/* SPDX-License-Identifier: GPL-2.0-only */
/* NativeMaliPower against fake SPM/INFRACFG/TOPCKGEN/MFG_CFG/PWRAP blocks.
 *
 * What this qualifies: the exact vendor order (power before bus-protect
 * release; bus protect before power-down), the PWR_CON bit steps, polling and
 * timeouts, rollback, the topckgen mux/gate writes, the MFG_CFG gate, and the
 * read-only probe. What it cannot qualify: the chip. Real bring-up needs the
 * phone, and writes stay compiled out until NATIVE_MALI_M2_WRITES is set. */
#include "FakeMaliPlatform.h"
#include "NativeMaliPower.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)

namespace {

constexpr uint32_t kPowerOn = NativeMaliPower::PowerConOn |
    NativeMaliPower::PowerConOnSecond;
constexpr uint32_t kPowered = kPowerOn | NativeMaliPower::PowerConResetB;

struct Rig {
    FakeSpm spm;
    FakeInfra infra;
    FakeTopckgen top;
    FakeMfgcfg mfgcfg;
    FakePwrap pwrap;
    NativeMaliPower power;

    Rig()
    {
        pwrap.setPmic(NativeMaliMt6358::VgpuEnable, 1);
        pwrap.setPmic(NativeMaliMt6358::VgpuVosel, 0);
        pwrap.setPmic(NativeMaliMt6358::VsramEnable, 1);
        pwrap.setPmic(NativeMaliMt6358::VsramVosel, 0);
        /* Wrong parent, mux gate still closed, clock gate still closed. */
        top.memory[0x50 / 4] = 0x3 | 0x80;
        mfgcfg.memory[0] = 1;
        NativeMaliPowerBlocks blocks{&spm, &infra, &top, &mfgcfg, &pwrap};
        CHECK(power.prepare(blocks) == kIOReturnSuccess);
    }

    uint32_t control(uint32_t offset) const { return spm.memory[offset / 4]; }
};

void testPrepare()
{
    NativeMaliPower power;
    NativeMaliPowerBlocks nothing;
    CHECK(power.prepare(nothing) == kIOReturnBadArgument);
    CHECK(!power.prepared());

    FakeSpm spm;
    FakeInfra infra;
    FakeTopckgen top;
    FakeMfgcfg mfgcfg;
    FakePwrap pwrap;
    NativeMaliPowerBlocks blocks{&spm, &infra, &top, &mfgcfg, &pwrap};
    CHECK(power.prepare(blocks) == kIOReturnSuccess);
    CHECK(power.prepared());
    CHECK(power.prepare(blocks) == kIOReturnSuccess);

    FakeSpm other;
    blocks.spm = &other;
    CHECK(power.prepare(blocks) == kIOReturnBusy);
}

void testProbe()
{
    Rig rig;
    rig.spm.deriveStatus = false;
    rig.spm.memory[0x180 / 4] = 0x8C00;
    rig.spm.memory[0x184 / 4] = 0x8400;
    rig.infra.memory[0x220 / 4] = NativeMaliPower::MfgProtectStep1;
    rig.top.memory[0x50 / 4] = 0x83;
    rig.top.memory[0x4 / 4] = 0x10;
    rig.mfgcfg.memory[0] = 1;
    rig.pwrap.setPmic(NativeMaliMt6358::VgpuVosel, 18);
    rig.pwrap.setPmic(NativeMaliMt6358::VsramVosel, 56);

    NativeMaliPowerObservation seen;
    CHECK(rig.power.probe(&seen) == kIOReturnSuccess);
    CHECK(seen.spmStatus == 0x8C00);
    CHECK(seen.spmStatus2 == 0x8400);
    CHECK(seen.infraProtectEnable == NativeMaliPower::MfgProtectStep1);
    CHECK(seen.infraProtectStatus1 == NativeMaliPower::MfgProtectStep1);
    CHECK(seen.topMfgClockConfig == 0x83);
    CHECK(seen.topClockUpdate == 0x10);
    CHECK(seen.mfgClockGateStatus == 1);
    CHECK(seen.vgpu.microvolts == 612500 && seen.vgpu.enabled);
    CHECK(seen.vsram.microvolts == 850000 && seen.vsram.enabled);

    NativeMaliPower lonely;
    CHECK(lonely.probe(&seen) == kIOReturnNotReady);
    CHECK(rig.power.probe(nullptr) == kIOReturnBadArgument);

    /* A wrapper that has not finished initialising must make the probe fail,
     * not return a plausible zero. */
    Rig broken;
    broken.pwrap.initDone = false;
    CHECK(broken.power.probe(&seen) == kIOReturnNotReady);
}

void testGpuReadable()
{
    const uint32_t all = NativeMaliPower::GpuReadMissingDomains |
        NativeMaliPower::GpuReadMissingClockMux |
        NativeMaliPower::GpuReadMissingClockGate;
    const uint32_t controls[] = {
        NativeMaliPower::SpmMfgAsyncPowerCon,
        NativeMaliPower::SpmMfgPowerCon,
        NativeMaliPower::SpmMfgCore0PowerCon,
        NativeMaliPower::SpmMfgCore1PowerCon,
    };

    Rig rig;
    uint32_t missing = 0xFFFF;
    /* Rig starts with every domain off, the mux gated and BG3D closed. */
    CHECK(rig.power.gpuReadable(&missing) == kIOReturnNotReady);
    CHECK(missing == all);

    for(uint32_t control : controls)
        rig.spm.memory[control / 4] = kPowerOn;
    missing = 0;
    CHECK(rig.power.gpuReadable(&missing) == kIOReturnNotReady);
    CHECK(missing == (NativeMaliPower::GpuReadMissingClockMux |
        NativeMaliPower::GpuReadMissingClockGate));

    /* Only the mux gate bit matters: a bootloader may have left the GPU on
     * syspll_d3, and reading registers is safe on any enabled parent. */
    rig.top.memory[0x50 / 4] = 2;
    CHECK(rig.power.gpuReadable(&missing) == kIOReturnNotReady);
    CHECK(missing == NativeMaliPower::GpuReadMissingClockGate);

    rig.mfgcfg.memory[0] = 0;
    missing = all;
    CHECK(rig.power.gpuReadable(&missing) == kIOReturnSuccess);
    CHECK(missing == 0);
    CHECK(rig.power.gpuReadable() == kIOReturnSuccess);

    /* One domain off in either status register is enough to refuse. */
    rig.spm.memory[NativeMaliPower::SpmMfgCore1PowerCon / 4] = 0;
    CHECK(rig.power.gpuReadable(&missing) == kIOReturnNotReady);
    CHECK(missing == NativeMaliPower::GpuReadMissingDomains);
    rig.spm.memory[NativeMaliPower::SpmMfgCore1PowerCon / 4] = kPowerOn;

    const uint32_t domains = NativeMaliPower::SpmPowerStatusMfg |
        NativeMaliPower::SpmPowerStatusMfgAsync |
        NativeMaliPower::SpmPowerStatusMfgCore0 |
        NativeMaliPower::SpmPowerStatusMfgCore1;
    rig.spm.deriveStatus = false;
    rig.spm.memory[NativeMaliPower::SpmPowerStatus / 4] = domains;
    rig.spm.memory[NativeMaliPower::SpmPowerStatus2 / 4] = domains;
    CHECK(rig.power.gpuReadable(&missing) == kIOReturnSuccess);
    rig.spm.memory[NativeMaliPower::SpmPowerStatus2 / 4] =
        domains & ~NativeMaliPower::SpmPowerStatusMfgCore1;
    CHECK(rig.power.gpuReadable(&missing) == kIOReturnNotReady);
    CHECK(missing == NativeMaliPower::GpuReadMissingDomains);
    rig.spm.deriveStatus = true;

    /* A failed read is an error, not a "not ready" verdict. */
    rig.spm.failReadAt = NativeMaliPower::SpmPowerStatus;
    CHECK(rig.power.gpuReadable(&missing) == kIOReturnIOError);
    rig.spm.failReadAt = -1;
    rig.top.failReadAt = NativeMaliPower::TopMfgClockConfig;
    CHECK(rig.power.gpuReadable(&missing) == kIOReturnIOError);
    rig.top.failReadAt = -1;
    rig.mfgcfg.failReadAt = NativeMaliPower::MfgClockGateStatus;
    CHECK(rig.power.gpuReadable(&missing) == kIOReturnIOError);

    NativeMaliPower lonely;
    missing = 0xFFFF;
    CHECK(lonely.gpuReadable(&missing) == kIOReturnNotReady);
    CHECK(missing == 0);
}

void testWaitFor()
{
    FakeRegisterBlock block;
    block.memory[0x10 / 4] = 0x4;
    CHECK(NativeMaliPower::waitFor(&block, 0x10, 0x4, 100, 10, nullptr) ==
        kIOReturnSuccess);
    CHECK(block.delays.empty());

    block.memory[0x10 / 4] = 0;
    CHECK(NativeMaliPower::waitFor(&block, 0x10, 0x4, 0, 10, nullptr) ==
        kIOReturnTimeout);
    block.delays.clear();
    uint32_t last = 0;
    CHECK(NativeMaliPower::waitFor(&block, 0x10, 0x4, 30, 10, &last) ==
        kIOReturnTimeout);
    CHECK(block.delays.size() == 3);
    CHECK(last == 0);

    CHECK(NativeMaliPower::waitFor(&block, 0x10, 0, 100, 10, nullptr) ==
        kIOReturnBadArgument);
    CHECK(NativeMaliPower::waitFor(&block, 0x10, 0x4, 100, 0, nullptr) ==
        kIOReturnBadArgument);
    CHECK(NativeMaliPower::waitFor(nullptr, 0x10, 0x4, 100, 10, nullptr) ==
        kIOReturnBadArgument);

    block.delayResult = kIOReturnNoResources;
    CHECK(NativeMaliPower::waitFor(&block, 0x10, 0x4, 100, 10, nullptr) ==
        kIOReturnNoResources);
}

void testClockSource()
{
    Rig rig;
    CHECK(rig.power.selectMfgClockSource() == kIOReturnSuccess);
    CHECK(rig.top.memory[0x50 / 4] == 0);
    CHECK(rig.top.writes.size() == 4);
    CHECK(rig.top.writes[0].offset == NativeMaliPower::TopMfgClockConfigClear);
    CHECK(rig.top.writes[0].value == NativeMaliPower::TopMfgMuxGate);
    CHECK(rig.top.writes[1].offset == NativeMaliPower::TopMfgClockConfigClear);
    CHECK(rig.top.writes[1].value == NativeMaliPower::TopMfgSelectMask);
    CHECK(rig.top.writes[2].offset == NativeMaliPower::TopMfgClockConfigSet);
    CHECK(rig.top.writes[2].value == 0);
    CHECK(rig.top.writes[3].offset == NativeMaliPower::TopClockUpdate);
    CHECK(rig.top.writes[3].value == NativeMaliPower::TopMfgUpdate);

    Rig failed;
    failed.top.failWriteAt = NativeMaliPower::TopMfgClockConfigSet;
    CHECK(failed.power.selectMfgClockSource() == kIOReturnIOError);
}

void testClockGate()
{
    Rig rig;
    CHECK(rig.power.enableMfgClockGate() == kIOReturnSuccess);
    CHECK(rig.mfgcfg.memory[0] == 0);
    CHECK(rig.mfgcfg.countWrites(NativeMaliPower::MfgClockGateClear) == 1);

    rig.mfgcfg.writes.clear();
    CHECK(rig.power.enableMfgClockGate() == kIOReturnSuccess);
    CHECK(rig.mfgcfg.writes.empty());

    Rig stuck;
    stuck.mfgcfg.ignoreClear = true;
    CHECK(stuck.power.enableMfgClockGate() == kIOReturnIOError);

    Rig failed;
    failed.mfgcfg.failWriteAt = NativeMaliPower::MfgClockGateClear;
    CHECK(failed.power.enableMfgClockGate() == kIOReturnIOError);
}

void testDomains()
{
    Rig rig;
    rig.spm.pollsBeforeUp = 2;
    rig.infra.watchWord = &rig.spm.memory[NativeMaliPower::SpmMfgPowerCon / 4];
    CHECK(rig.power.powerOnDomains(200) == kIOReturnSuccess);
    CHECK(rig.spm.delays.size() == 8);   /* two polls for each of four domains */

    const uint32_t controls[] = {
        NativeMaliPower::SpmMfgAsyncPowerCon,
        NativeMaliPower::SpmMfgPowerCon,
        NativeMaliPower::SpmMfgCore0PowerCon,
        NativeMaliPower::SpmMfgCore1PowerCon,
    };
    for(uint32_t control : controls) {
        const uint32_t word = rig.control(control);
        CHECK((word & 0x1F) == kPowered);
        CHECK((word & NativeMaliPower::PowerConSramPowerDown) == 0);
    }
    CHECK(rig.infra.memory[0x220 / 4] == 0);

    /* Vendor order: MFG is powered first, then bus protection is released.
     * At the moment PROTECTEN_CLR ran, PWR_ON was set and SRAM_PDN was not. */
    CHECK(rig.infra.sawProtectClear);
    CHECK((rig.infra.watchAtProtectClear & kPowerOn) == kPowerOn);
    CHECK((rig.infra.watchAtProtectClear &
        NativeMaliPower::PowerConSramPowerDown) == 0);
}

void testAlreadyOn()
{
    Rig rig;
    const uint32_t controls[] = {
        NativeMaliPower::SpmMfgAsyncPowerCon,
        NativeMaliPower::SpmMfgPowerCon,
        NativeMaliPower::SpmMfgCore0PowerCon,
        NativeMaliPower::SpmMfgCore1PowerCon,
    };
    for(uint32_t control : controls)
        rig.spm.memory[control / 4] = kPowered;
    CHECK(rig.power.powerOnDomains(200) == kIOReturnSuccess);
    CHECK(rig.spm.delays.empty());
    for(uint32_t control : controls)
        CHECK((rig.control(control) & 0x1F) == kPowered);
    CHECK(rig.infra.memory[0x220 / 4] == 0);
}

void testTimeoutRollsBack()
{
    Rig rig;
    rig.spm.stuckDomain = NativeMaliPower::SpmMfgPowerCon;
    rig.infra.watchWord = &rig.spm.memory[NativeMaliPower::SpmMfgPowerCon / 4];
    CHECK(rig.power.powerOnDomains(100) == kIOReturnTimeout);

    /* The failing MFG domain and the async domain that had already come up are
     * both off; the cores were never touched. */
    CHECK((rig.control(NativeMaliPower::SpmMfgAsyncPowerCon) & kPowerOn) == 0);
    CHECK((rig.control(NativeMaliPower::SpmMfgPowerCon) & kPowerOn) == 0);
    CHECK(rig.control(NativeMaliPower::SpmMfgCore0PowerCon) == 0);
    CHECK(rig.control(NativeMaliPower::SpmMfgCore1PowerCon) == 0);

    /* MFG is bus-protected before it is powered down, not after. */
    CHECK(rig.infra.sawProtectSet);
    CHECK((rig.infra.watchAtProtectSet & kPowerOn) == kPowerOn);
}

void testBringUp()
{
    CHECK(NativeMaliPower::kWriteAuthorized);
    Rig rig;
    CHECK(rig.power.bringUp(200) == kIOReturnSuccess);

    CHECK(rig.pwrap.getPmic(NativeMaliMt6358::VsramVosel) == 56);
    CHECK(rig.pwrap.getPmic(NativeMaliMt6358::VgpuVosel) == 18);
    CHECK(rig.top.memory[0x50 / 4] == 0);
    CHECK(rig.mfgcfg.memory[0] == 0);
    uint32_t status = 0;
    CHECK(rig.spm.read32(NativeMaliPower::SpmPowerStatus, &status) ==
        kIOReturnSuccess);
    const uint32_t domains =
        NativeMaliPower::SpmPowerStatusMfg |
        NativeMaliPower::SpmPowerStatusMfgAsync |
        NativeMaliPower::SpmPowerStatusMfgCore0 |
        NativeMaliPower::SpmPowerStatusMfgCore1;
    CHECK((status & domains) == domains);
}

void testGateFailureQuiesces()
{
    Rig rig;
    rig.mfgcfg.failWriteAt = NativeMaliPower::MfgClockGateClear;
    CHECK(rig.power.bringUp(200) == kIOReturnIOError);

    const uint32_t controls[] = {
        NativeMaliPower::SpmMfgAsyncPowerCon,
        NativeMaliPower::SpmMfgPowerCon,
        NativeMaliPower::SpmMfgCore0PowerCon,
        NativeMaliPower::SpmMfgCore1PowerCon,
    };
    for(uint32_t control : controls)
        CHECK((rig.control(control) & kPowerOn) == 0);
    CHECK(rig.mfgcfg.memory[0] == 1);
}

/* The vendor's clk_disable_unprepare(subsys_mfg_cg) callback: write 1 to the
 * gate's SET register and require the status bit to read gated afterwards. */
void testClockGateClose()
{
    Rig rig;
    CHECK(rig.power.enableMfgClockGate() == kIOReturnSuccess);
    CHECK(rig.mfgcfg.memory[0] == 0);

    CHECK(rig.power.disableMfgClockGate() == kIOReturnSuccess);
    CHECK(rig.mfgcfg.memory[0] == NativeMaliPower::MfgClockGateBg3d);
    CHECK(rig.mfgcfg.countWrites(NativeMaliPower::MfgClockGateSet) == 1);
    CHECK(rig.mfgcfg.writes.back().value == NativeMaliPower::MfgClockGateBg3d);

    /* Closing a gate that is already closed is a write plus a passing check,
     * not an error: the vendor's clk_disable_unprepare() is unconditional. */
    CHECK(rig.power.disableMfgClockGate() == kIOReturnSuccess);

    Rig stuck;
    stuck.mfgcfg.memory[0] = 0;   /* gate open, so the SET has something to do */
    stuck.mfgcfg.ignoreSet = true;
    CHECK(stuck.power.disableMfgClockGate() == kIOReturnIOError);

    Rig failed;
    failed.mfgcfg.failWriteAt = NativeMaliPower::MfgClockGateSet;
    CHECK(failed.power.disableMfgClockGate() == kIOReturnIOError);
}

/* mt_gpufreq_disable_MTCMOS(): core1, core0, mfg, async, i.e. exactly the
 * reverse of the power-on order. The rig-wide sequence counter proves it across
 * the four registers. */
void testPowerDownOrder()
{
    Rig rig;
    uint32_t sequence = 0;
    rig.spm.sequence = &sequence;
    CHECK(rig.power.powerOnDomains(200) == kIOReturnSuccess);
    CHECK(rig.power.powerDownDomains(200) == kIOReturnSuccess);

    const uint32_t order[] = {
        NativeMaliPower::SpmMfgCore1PowerCon,
        NativeMaliPower::SpmMfgCore0PowerCon,
        NativeMaliPower::SpmMfgPowerCon,
        NativeMaliPower::SpmMfgAsyncPowerCon,
    };
    uint32_t previous = 0;
    for(uint32_t control : order) {
        /* The first write to each control that clears PWR_ON is the power-down
         * step for that domain. */
        uint32_t seq = 0;
        for(const FakeRegisterBlock::Write &write : rig.spm.writes)
            if(write.offset == control && (write.value & kPowerOn) == 0) {
                seq = write.seq;
                break;
            }
        CHECK(seq != 0);
        CHECK(seq > previous);
        previous = seq;
        CHECK((rig.control(control) & kPowerOn) == 0);
        CHECK((rig.control(control) & NativeMaliPower::PowerConIso) != 0);
        CHECK((rig.control(control) & NativeMaliPower::PowerConResetB) == 0);
    }

    /* And the domains really are off in both status registers. */
    CHECK(rig.power.gpuReadable(nullptr) == kIOReturnNotReady);
}

/* powerDownDomains() keeps going after a step fails, so one wedged domain does
 * not leave the others powered (the shape of panfrost_gpu_power_off()). */
void testPowerDownKeepsGoing()
{
    Rig rig;
    CHECK(rig.power.powerOnDomains(200) == kIOReturnSuccess);
    rig.spm.stuckOffDomain = NativeMaliPower::SpmMfgPowerCon;
    CHECK(rig.power.powerDownDomains(50) == kIOReturnTimeout);

    /* core1 and core0 are off even though mfg refused to power down. */
    CHECK((rig.control(NativeMaliPower::SpmMfgCore1PowerCon) & kPowerOn) == 0);
    CHECK((rig.control(NativeMaliPower::SpmMfgCore0PowerCon) & kPowerOn) == 0);
    /* async, the last domain in the order, was still attempted. */
    CHECK((rig.control(NativeMaliPower::SpmMfgAsyncPowerCon) & kPowerOn) == 0);
}

/* A second power cycle after a full teardown must work. The ack bit in
 * PWR_CON is a read-only status bit (SRAM_PDN_ACK follows SRAM_PDN); a
 * read-modify-write must not latch it, or the next bring-up would wait for an
 * ack that can never clear. */
void testPowerCycle()
{
    Rig rig;
    for(int round = 0; round < 2; ++round) {
        CHECK(rig.power.bringUp(200) == kIOReturnSuccess);
        CHECK(rig.power.gpuReadable(nullptr) == kIOReturnSuccess);
        CHECK(rig.power.tearDown(200) == kIOReturnSuccess);
        CHECK(rig.power.gpuReadable(nullptr) == kIOReturnNotReady);
    }
    CHECK((rig.spm.memory[NativeMaliPower::SpmMfgPowerCon / 4] &
        NativeMaliPower::PowerConSramPowerDownAck) == 0);
}

void testTearDown()
{
    Rig rig;
    uint32_t sequence = 0;
    rig.mfgcfg.sequence = &sequence;
    rig.spm.sequence = &sequence;
    CHECK(rig.power.bringUp(200) == kIOReturnSuccess);
    CHECK(rig.mfgcfg.memory[0] == 0);
    const size_t pwrapWrites = rig.pwrap.writes.size();
    CHECK(rig.power.tearDown(200) == kIOReturnSuccess);

    /* Vendor order: clock gate first (mt_gpufreq_disable_CG), then the four
     * domains (mt_gpufreq_disable_MTCMOS). */
    CHECK(rig.mfgcfg.memory[0] == NativeMaliPower::MfgClockGateBg3d);
    const uint32_t gateSeq = rig.mfgcfg.writes.back().seq;
    uint32_t firstDomainSeq = UINT32_MAX;
    for(const FakeRegisterBlock::Write &write : rig.spm.writes)
        if((write.value & kPowerOn) == 0 && write.seq != 0)
            firstDomainSeq = std::min(firstDomainSeq, write.seq);
    CHECK(gateSeq < firstDomainSeq);
    CHECK((rig.control(NativeMaliPower::SpmMfgPowerCon) & kPowerOn) == 0);
    CHECK((rig.control(NativeMaliPower::SpmMfgAsyncPowerCon) & kPowerOn) == 0);

    /* The rails are deliberately left alone: this driver never enabled them, so
     * tearDown() does not write the PMIC at all. */
    CHECK(rig.pwrap.getPmic(NativeMaliMt6358::VgpuVosel) == 18);
    CHECK(rig.pwrap.getPmic(NativeMaliMt6358::VsramVosel) == 56);
    CHECK(rig.pwrap.writes.size() == pwrapWrites);

    /* Idempotent enough to retry: a second tearDown still succeeds. */
    CHECK(rig.power.tearDown(200) == kIOReturnSuccess);
}

/* A gate that will not close does not stop the domains from being powered down;
 * the gate error is the one reported. */
void testTearDownGateFailureStillPowersDown()
{
    Rig rig;
    CHECK(rig.power.bringUp(200) == kIOReturnSuccess);
    rig.mfgcfg.failWriteAt = NativeMaliPower::MfgClockGateSet;
    CHECK(rig.power.tearDown(200) == kIOReturnIOError);
    CHECK((rig.control(NativeMaliPower::SpmMfgCore1PowerCon) & kPowerOn) == 0);
    CHECK((rig.control(NativeMaliPower::SpmMfgAsyncPowerCon) & kPowerOn) == 0);
}

} // namespace

int main()
{
    testPrepare();
    testProbe();
    testGpuReadable();
    testWaitFor();
    testClockSource();
    testClockGate();
    testDomains();
    testAlreadyOn();
    testTimeoutRollsBack();
    testBringUp();
    testGateFailureQuiesces();
    testClockGateClose();
    testPowerDownOrder();
    testPowerDownKeepsGoing();
    testPowerCycle();
    testTearDown();
    testTearDownGateFailureStillPowersDown();
    std::puts("PASS: read-only probe, voltage->clock->power->gate bring-up, vendor domain order, bus-protect before power-down, polling/timeout, rollback, gate verification, the read gate for 0x13040000, the reverse teardown (gate before domains, all steps attempted) and a second power cycle");
    return 0;
}
