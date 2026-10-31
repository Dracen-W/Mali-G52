/* SPDX-License-Identifier: GPL-2.0-only */
/* The order and the rollback; every register fact is in the files it calls.
 * The citations for both are in NativeMaliBringup.h. */
#include "NativeMaliBringup.h"

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

    /* M2. bringUp() already rolls its own partial writes back. */
    result = power_.bringUp();
    if(result != kIOReturnSuccess)return result;
    powered_ = true;

    /* M3: identify before allocating an address space, the order
     * panfrost_device_init() uses (gpu_init at :312, mmu_init at :316). */
    result = gpu_.probe(&observation_);
    if(state)state->gpu = observation_;
    if(result != kIOReturnSuccess) {
        quiesce(teardownError);
        return result;
    }

    /* panfrost_device_reset(): reset, then power on (panfrost_device.c:514-516).
     * softReset() refuses without a successful probe, and powerOn() refuses
     * without a successful reset, so this order is also enforced downwards. */
    result = gpu_.softReset();
    if(result != kIOReturnSuccess) {
        quiesce(teardownError);
        return result;
    }
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
