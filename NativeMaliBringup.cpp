/* SPDX-License-Identifier: GPL-2.0-only */
/* The order and the rollback; every register fact is in the files it calls.
 * The citations for both are in NativeMaliBringup.h. */
#include "NativeMaliBringup.h"

extern "C" void native_gop_text(unsigned line, const char *text) __attribute__((weak));

namespace {
void stageMark(unsigned line, const char *text)
{
    if(native_gop_text)native_gop_text(line, text);
}
} // namespace

IOReturn NativeMaliBringup::prepare(const NativeMaliBringupBlocks &blocks)
{
    if(!blocks.gpu)return kIOReturnBadArgument;
    IOReturn result = power_.prepare(blocks.power);
    if(result != kIOReturnSuccess)return result;
    result = gpu_.prepare(blocks.gpu, &power_);
    if(result != kIOReturnSuccess)return result;
    /* The MMU and the job slots share the GPU window, so the same target is
     * handed to the MMU; the slots are not prepared here (see the header). */
    return mmu_.prepare(blocks.gpu, &power_);
}

bool NativeMaliBringup::prepared() const
{
    return power_.prepared() && gpu_.prepared() && mmu_.prepared();
}

IOReturn NativeMaliBringup::start(NativeMaliBringupState *state,
    IOReturn *teardownError)
{
    if(teardownError)*teardownError = kIOReturnSuccess;
    if(!prepared())return kIOReturnNotReady;
    if(powered_)return kIOReturnBusy;

    /* M1: read the platform before touching a configuration register. A probe
     * that fails means the SPM/PMIC reads this driver depends on did not
     * answer, and nothing may be written on top of that (roadmap rule 2). */
    NativeMaliPowerObservation powerObservation;
    IOReturn result = power_.probe(&powerObservation);
    if(result != kIOReturnSuccess)return result;
    if(state)state->power = powerObservation;

    /* M2, one step at a time, probing after each step. bringUp() bundles the
     * same order, but this keeps the intermediate values visible on the phone
     * and leaves the failing step in `powerStageStatus`. */
    /* The MT6768 preloader leaves VGPU and VSRAM_GPU enabled. The validated
     * redwood_mali bring-up does not touch the PMIC, so do not issue the M2
     * pwrap voltage reads or writes here. */
    stageMark(9, "M2V preloader");
    if(state)state->powerStagesDone = 0;

    stageMark(10, "M2C preloader");
    stageMark(14, "CK clock preloader");
    /* TEMPORARY: the MT6768 preloader leaves mfg_sel on syspll_d3. Writing
     * CLK_CFG_1_SET blocks this board's topckgen, so rely on the preloader
     * value for this bring-up attempt. Remove after the GPU clock is verified. */

    stageMark(11, "M2D");
    stageMark(15, "CK domains");
    result = power_.powerOnDomains();
    if(result != kIOReturnSuccess)return result;

    stageMark(12, "M2G");
    stageMark(16, "CK gate");
    result = power_.enableMfgClockGate();
    if(result != kIOReturnSuccess) {
        power_.powerDownDomains();
        return result;
    }
    powered_ = true;

    /* M3: identify before allocating an address space, the order
     * panfrost_device_init() uses (gpu_init at :312, mmu_init at :316). */
    stageMark(17, "CK GPU ID");
    result = gpu_.probe(&observation_);
    if(state)state->gpu = observation_;
    if(result != kIOReturnSuccess) {
        quiesce(teardownError);
        return result;
    }

    /* panfrost_device_reset(): reset, then power on (panfrost_device.c:514-516).
     * softReset() refuses without a successful probe, and powerOn() refuses
     * without a successful reset, so this order is also enforced downwards. */
    stageMark(18, "CK GPU reset");
    result = gpu_.softReset();
    if(result != kIOReturnSuccess) {
        quiesce(teardownError);
        return result;
    }
    stageMark(19, "CK GPU cores");
    result = gpu_.powerOn();
    if(result != kIOReturnSuccess) {
        /* powerOn() has already quiesced L2/shader/tiler itself. */
        quiesce(teardownError);
        return result;
    }
    gpuOn_ = true;

    /* M4: panfrost_mmu_reset() after power-on (panfrost_device.c:517). The
     * present mask comes from the probe and is a hardware fact, so it is
     * recorded before the allocation bitmap is cleared. */
    stageMark(20, "CK MMU");
    result = mmu_.setPresent(observation_.asPresent);
    if(result != kIOReturnSuccess) {
        quiesce(teardownError);
        return result;
    }
    mmu_.resetAllocation();
    return kIOReturnSuccess;
}

IOReturn NativeMaliBringup::quiesce(IOReturn *teardownError)
{
    IOReturn first = kIOReturnSuccess;
    auto record = [&first](IOReturn result) {
        if(result != kIOReturnSuccess && first == kIOReturnSuccess)first = result;
    };

    if(gpuOn_) {
        const IOReturn result = gpu_.powerOff();
        record(result);
        if(result == kIOReturnSuccess)gpuOn_ = false;
    }
    if(powered_) {
        /* Gate first, then the domains (NativeMaliPower::tearDown()). */
        const IOReturn result = power_.tearDown();
        record(result);
        if(result == kIOReturnSuccess)powered_ = false;
    }

    if(teardownError)*teardownError = first;
    return first;
}

IOReturn NativeMaliBringup::stop(Jobs jobs, IOReturn *teardownError)
{
    if(teardownError)*teardownError = kIOReturnSuccess;
    /* panfrost_device_runtime_suspend() refuses to suspend with work in flight
     * (panfrost_device.c:553-554). Nothing is written in that case. */
    if(jobs == Jobs::Outstanding)return kIOReturnBusy;
    /* An address space belongs to its owner (see the header): powering the
     * block down while one is still allocated would leave the owner with a
     * number the hardware no longer honours, so this refuses instead. */
    if(mmu_.allocatedMask() != 0)return kIOReturnBusy;
    return quiesce(teardownError);
}
