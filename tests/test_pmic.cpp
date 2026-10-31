/* SPDX-License-Identifier: GPL-2.0-only */
/* WACS2 transport and MT6358 rails against the fake wrapper. This qualifies
 * the protocol, the selector arithmetic, ordering and read-back; it cannot
 * qualify the PMIC, which is only reachable from the phone. */
#include "FakeMaliPlatform.h"
#include "NativeMaliMt6358.h"
#include "NativeMaliPwrap.h"
#include <cstdio>
#include <cstdlib>

#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)

namespace {

/* PMIC writes are the commands with bit 31 set; reads have it clear. */
std::vector<uint16_t> pmicWriteAddresses(const FakePwrap &fake)
{
    std::vector<uint16_t> addresses;
    for(const FakeRegisterBlock::Write &write : fake.writes) {
        if(write.offset != 0xC20 || !(write.value & 0x80000000u))continue;
        addresses.push_back(static_cast<uint16_t>(
            ((write.value >> 16) & 0xFFFF) << 1));
    }
    return addresses;
}

void testSelectors()
{
    uint16_t selector = 0xFFFF;
    uint32_t microvolts = 0;
    CHECK(NativeMaliMt6358::encode(500000, &selector) && selector == 0);
    CHECK(NativeMaliMt6358::encode(612500, &selector) && selector == 18);
    CHECK(NativeMaliMt6358::encode(850000, &selector) && selector == 56);
    CHECK(NativeMaliMt6358::encode(1293750, &selector) && selector == 127);
    CHECK(!NativeMaliMt6358::encode(499999, &selector));
    CHECK(!NativeMaliMt6358::encode(1293751, &selector));
    CHECK(!NativeMaliMt6358::encode(612501, &selector));
    CHECK(!NativeMaliMt6358::encode(612500, nullptr));

    CHECK(NativeMaliMt6358::decode(0, &microvolts) && microvolts == 500000);
    CHECK(NativeMaliMt6358::decode(18, &microvolts) && microvolts == 612500);
    CHECK(NativeMaliMt6358::decode(56, &microvolts) && microvolts == 850000);
    CHECK(NativeMaliMt6358::decode(127, &microvolts) && microvolts == 1293750);
    CHECK(!NativeMaliMt6358::decode(128, &microvolts));
      }

void testWrapperRead()
{
    FakePwrap fake;
    fake.setPmic(0x1508, 0x0001);
    NativeMaliPwrap pwrap;
    CHECK(!pwrap.prepared());
    CHECK(pwrap.prepare(&fake) == kIOReturnSuccess);
    CHECK(pwrap.prepared());

    uint16_t value = 0;
    CHECK(pwrap.read(0x1508, &value) == kIOReturnSuccess);
    CHECK(value == 0x0001);
    /* command = (0x1508 >> 1) << 16; reads have bit 31 clear. */
    CHECK(fake.writes.front().offset == 0xC20);
    CHECK(fake.writes.front().value == 0x0A840000u);
    CHECK(fake.countWrites(0xC28) == 1);
    CHECK(fake.writes.back().offset == 0xC28);

    /* A second read still works: VLDCLR put the FSM back to idle. */
    fake.setPmic(0x1508, 0x0042);
    CHECK(pwrap.read(0x1508, &value) == kIOReturnSuccess);
    CHECK(value == 0x0042);
}

void testWrapperWrite()
{
    FakePwrap fake;
    NativeMaliPwrap pwrap;
    CHECK(pwrap.prepare(&fake) == kIOReturnSuccess);
    CHECK(pwrap.write(0x1526, 0x0012) == kIOReturnSuccess);
    CHECK(fake.getPmic(0x1526) == 0x0012);
    CHECK(fake.writes.size() == 1);
    CHECK(fake.writes.front().offset == 0xC20);
    CHECK(fake.writes.front().value == 0x8A930012u);
}

void testWrapperFailures()
{
    {
        FakePwrap fake;
        fake.initDone = false;
        NativeMaliPwrap pwrap;
        CHECK(pwrap.prepare(&fake) == kIOReturnSuccess);
        uint16_t value = 0;
        CHECK(pwrap.read(0x1508, &value) == kIOReturnNotReady);
        CHECK(value == 0);
    }
    {
        FakePwrap fake;
        fake.neverReady = true;
        NativeMaliPwrap pwrap;
        CHECK(pwrap.prepare(&fake) == kIOReturnSuccess);
        uint16_t value = 0;
        CHECK(pwrap.read(0x1508, &value) == kIOReturnTimeout);
        CHECK(pwrap.write(0x1508, 1) == kIOReturnSuccess);
    }
    {
        /* A completed but unacknowledged read must be cleared before the next
         * command, not mistaken for the next command's answer. */
        FakePwrap fake;
        NativeMaliPwrap pwrap;
        CHECK(pwrap.prepare(&fake) == kIOReturnSuccess);
        fake.makeStale(0x1508, 0x0055);
        uint16_t value = 0;
        CHECK(pwrap.read(0x1508, &value) == kIOReturnSuccess);
        CHECK(value == 0x0055);
        CHECK(fake.countWrites(0xC28) == 2);
    }
    {
        FakePwrap fake;
        NativeMaliPwrap pwrap;
        CHECK(pwrap.prepare(&fake) == kIOReturnSuccess);
        fake.failReadAt = 0xC24;
        uint16_t value = 0;
        CHECK(pwrap.read(0x1508, &value) == kIOReturnIOError);
    }
    {
        NativeMaliPwrap pwrap;
        uint16_t value = 0;
        CHECK(pwrap.read(0, &value) == kIOReturnNotReady);
        CHECK(pwrap.write(0, 0) == kIOReturnNotReady);
    }
}

struct Rig {
    FakePwrap fake;
    NativeMaliPwrap pwrap;
    NativeMaliMt6358 pmic;

    Rig()
    {
        fake.setPmic(NativeMaliMt6358::VgpuEnable, 1);
        fake.setPmic(NativeMaliMt6358::VgpuVosel, 0);   /* 500000 uV */
        fake.setPmic(NativeMaliMt6358::VsramEnable, 1);
        fake.setPmic(NativeMaliMt6358::VsramVosel, 0);
        CHECK(pwrap.prepare(&fake) == kIOReturnSuccess);
        CHECK(pmic.prepare(&pwrap) == kIOReturnSuccess);
    }
};

void testFixedVoltage()
{
    Rig rig;
    const std::vector<uint16_t> order = pmicWriteAddresses(rig.fake);
    CHECK(order.empty());

    CHECK(rig.pmic.setFixed() == kIOReturnSuccess);
    CHECK(rig.fake.getPmic(NativeMaliMt6358::VgpuVosel) == 18);
    CHECK(rig.fake.getPmic(NativeMaliMt6358::VsramVosel) == 56);
    CHECK(rig.fake.getPmic(NativeMaliMt6358::VgpuEnable) == 1);
    CHECK(rig.fake.getPmic(NativeMaliMt6358::VsramEnable) == 1);

    NativeMaliMt6358::RailState state;
    CHECK(rig.pmic.readVgpu(&state) == kIOReturnSuccess);
    CHECK(state.microvolts == 612500 && state.enabled);
    CHECK(rig.pmic.readVsram(&state) == kIOReturnSuccess);
    CHECK(state.microvolts == 850000 && state.enabled);

    /* VSRAM_GPU is set before VGPU, and only the two VOSEL registers needed a
     * write because both rails were already enabled. */
    const std::vector<uint16_t> writes = pmicWriteAddresses(rig.fake);
    CHECK(writes.size() == 2);
    CHECK(writes[0] == NativeMaliMt6358::VsramVosel);
    CHECK(writes[1] == NativeMaliMt6358::VgpuVosel);

    /* Second call is a no-op: the rails are already at the fixed values, so no
     * PMIC write happens (the reads still go through the wrapper). */
    rig.fake.writes.clear();
    CHECK(rig.pmic.setFixed() == kIOReturnSuccess);
    CHECK(pmicWriteAddresses(rig.fake).empty());
}

void testRailsStartDisabled()
{
    Rig rig;
    rig.fake.setPmic(NativeMaliMt6358::VgpuEnable, 0);
    rig.fake.setPmic(NativeMaliMt6358::VsramEnable, 0);
    CHECK(rig.pmic.setFixed() == kIOReturnSuccess);
    CHECK(rig.fake.getPmic(NativeMaliMt6358::VgpuEnable) == 1);
    CHECK(rig.fake.getPmic(NativeMaliMt6358::VsramEnable) == 1);
    const std::vector<uint16_t> writes = pmicWriteAddresses(rig.fake);
    CHECK(writes.size() == 4);
    CHECK(writes[0] == NativeMaliMt6358::VsramVosel);
    CHECK(writes[1] == NativeMaliMt6358::VsramEnable);
    CHECK(writes[2] == NativeMaliMt6358::VgpuVosel);
    CHECK(writes[3] == NativeMaliMt6358::VgpuEnable);
}

void testReadbackMismatchRollsBack()
{
    Rig rig;
    rig.fake.dropPmicWriteAt = NativeMaliMt6358::VgpuVosel;
    CHECK(rig.pmic.setFixed() == kIOReturnIOError);
    /* VSRAM had already been raised to 850000; the failure must put it back. */
    CHECK(rig.fake.getPmic(NativeMaliMt6358::VsramVosel) == 0);
    CHECK(rig.fake.getPmic(NativeMaliMt6358::VgpuVosel) == 0);
}

void testOutOfRange()
{
    Rig rig;
    CHECK(rig.pmic.setVgpu(499999) == kIOReturnBadArgument);
    CHECK(rig.pmic.setVgpu(1293751) == kIOReturnBadArgument);
    CHECK(rig.pmic.setVsram(612501) == kIOReturnBadArgument);
    CHECK(rig.pmic.setVgpu(612500) == kIOReturnSuccess);
    CHECK(rig.fake.getPmic(NativeMaliMt6358::VgpuVosel) == 18);
}

void testUnprepared()
{
    NativeMaliPwrap pwrap;
    NativeMaliMt6358 pmic;
    CHECK(pmic.prepare(&pwrap) == kIOReturnBadArgument);
    NativeMaliMt6358::RailState state;
    CHECK(pmic.readVgpu(&state) == kIOReturnNotReady);
    CHECK(pmic.setFixed() == kIOReturnNotReady);
}

} // namespace

int main()
{
    testSelectors();
    testWrapperRead();
    testWrapperWrite();
    testWrapperFailures();
    testFixedVoltage();
    testRailsStartDisabled();
    testReadbackMismatchRollsBack();
    testOutOfRange();
    testUnprepared();
    std::puts("PASS: WACS2 read/write/FSM/timeout/stale-valid, MT6358 selector arithmetic, VSRAM-before-VGPU order, idempotence, enable order, read-back rollback and range refusals");
    return 0;
}
