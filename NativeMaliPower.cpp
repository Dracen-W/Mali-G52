/* SPDX-License-Identifier: GPL-2.0-only */
/* M2 orchestration. The sources for every offset and step are in
 * NativeMaliPower.h; this file only puts them in the vendor's order. */
#include "NativeMaliPower.h"

namespace {

/* Dependency order and the properties each domain needs. The vendor clock
 * driver expresses the same thing with clock parents (clk-mt6768-pg.c:3477-3485)
 * and the gpufreq driver enables them async, mfg, core0, core1
 * (mtk_gpufreq_core.c:439-459). */
constexpr uint32_t kDomainCount = 4;

} // namespace

const NativeMaliPower::Domain *NativeMaliPower::mfgDomains()
{
    static const Domain domains[kDomainCount] = {
        {SpmMfgAsyncPowerCon, SpmPowerStatusMfgAsync, false, false},
        {SpmMfgPowerCon, SpmPowerStatusMfg, true, false},
        {SpmMfgCore0PowerCon, SpmPowerStatusMfgCore0, false, true},
        {SpmMfgCore1PowerCon, SpmPowerStatusMfgCore1, false, true},
    };
    return domains;
}

IOReturn NativeMaliPower::prepare(const NativeMaliPowerBlocks &blocks)
{
    if(!blocks.spm || !blocks.infracfg || !blocks.topckgen || !blocks.mfgcfg ||
        !blocks.pwrap)
        return kIOReturnBadArgument;
    if(spm_) {
        const bool same = spm_ == blocks.spm && infracfg_ == blocks.infracfg &&
            topckgen_ == blocks.topckgen && mfgcfg_ == blocks.mfgcfg;
        return same ? kIOReturnSuccess : kIOReturnBusy;
    }
    IOReturn result = pwrap_.prepare(blocks.pwrap);
    if(result != kIOReturnSuccess)return result;
    result = pmic_.prepare(&pwrap_);
    if(result != kIOReturnSuccess)return result;
    spm_ = blocks.spm;
    infracfg_ = blocks.infracfg;
    topckgen_ = blocks.topckgen;
    mfgcfg_ = blocks.mfgcfg;
    return kIOReturnSuccess;
}

bool NativeMaliPower::prepared() const
{
    return spm_ && infracfg_ && topckgen_ && mfgcfg_ && pwrap_.prepared() &&
        pmic_.prepared();
}

IOReturn NativeMaliPower::readWord(NativeMaliRegisterTarget *target,
    uint32_t offset, uint32_t *value) const
{
    if(!target || !value)return kIOReturnBadArgument;
    return target->read32(offset, value);
}

IOReturn NativeMaliPower::writeWord(NativeMaliRegisterTarget *target,
    uint32_t offset, uint32_t value) const
{
    if(!target)return kIOReturnBadArgument;
    return target->write32(offset, value);
}

IOReturn NativeMaliPower::modify(NativeMaliRegisterTarget *target,
    uint32_t offset, uint32_t clear, uint32_t set) const
{
    uint32_t current = 0;
    IOReturn result = readWord(target, offset, &current);
    if(result != kIOReturnSuccess)return result;
    return writeWord(target, offset, (current & ~clear) | set);
}

IOReturn NativeMaliPower::waitFor(NativeMaliRegisterTarget *target,
    uint32_t offset, uint32_t mask, uint64_t timeout_us, uint32_t poll_us,
    uint32_t *last)
{
    if(!target)return kIOReturnBadArgument;
    if(!mask)return kIOReturnBadArgument;
    if(timeout_us && !poll_us)return kIOReturnBadArgument;
    uint32_t value = 0;
    IOReturn result = target->read32(offset, &value);
    if(result != kIOReturnSuccess)return result;
    if((value & mask) == mask) {
        if(last)*last = value;
        return kIOReturnSuccess;
    }
    for(uint64_t waited = 0; waited < timeout_us; waited += poll_us) {
        result = target->delay(poll_us);
        if(result != kIOReturnSuccess)return result;
        result = target->read32(offset, &value);
        if(result != kIOReturnSuccess)return result;
        if((value & mask) == mask) {
            if(last)*last = value;
            return kIOReturnSuccess;
        }
    }
    if(last)*last = value;
    return kIOReturnTimeout;
}

IOReturn NativeMaliPower::waitStatus(uint32_t mask, uint64_t timeout_us) const
{
    uint32_t status = 0;
    uint32_t status2 = 0;
    IOReturn result = readWord(spm_, SpmPowerStatus, &status);
    if(result != kIOReturnSuccess)return result;
    result = readWord(spm_, SpmPowerStatus2, &status2);
    if(result != kIOReturnSuccess)return result;
    if((status & mask) == mask && (status2 & mask) == mask)
        return kIOReturnSuccess;

    for(uint64_t waited = 0; waited < timeout_us; waited += DefaultPollUs) {
        result = spm_->delay(DefaultPollUs);
        if(result != kIOReturnSuccess)return result;
        result = readWord(spm_, SpmPowerStatus, &status);
        if(result != kIOReturnSuccess)return result;
        result = readWord(spm_, SpmPowerStatus2, &status2);
        if(result != kIOReturnSuccess)return result;
        if((status & mask) == mask && (status2 & mask) == mask)
            return kIOReturnSuccess;
    }
    return kIOReturnTimeout;
}

IOReturn NativeMaliPower::waitStatusOff(uint32_t mask, uint64_t timeout_us) const
{
    uint32_t status = 0;
    uint32_t status2 = 0;
    IOReturn result = readWord(spm_, SpmPowerStatus, &status);
    if(result != kIOReturnSuccess)return result;
    result = readWord(spm_, SpmPowerStatus2, &status2);
    if(result != kIOReturnSuccess)return result;
    if(!(status & mask) && !(status2 & mask))return kIOReturnSuccess;

    for(uint64_t waited = 0; waited < timeout_us; waited += DefaultPollUs) {
        result = spm_->delay(DefaultPollUs);
        if(result != kIOReturnSuccess)return result;
        result = readWord(spm_, SpmPowerStatus, &status);
        if(result != kIOReturnSuccess)return result;
        result = readWord(spm_, SpmPowerStatus2, &status2);
        if(result != kIOReturnSuccess)return result;
        if(!(status & mask) && !(status2 & mask))return kIOReturnSuccess;
    }
    return kIOReturnTimeout;
}

IOReturn NativeMaliPower::waitControl(uint32_t offset, uint32_t mask, bool set,
    uint64_t timeout_us) const
{
    uint32_t value = 0;
    IOReturn result = readWord(spm_, offset, &value);
    if(result != kIOReturnSuccess)return result;
    if(set ? ((value & mask) == mask) : ((value & mask) == 0))
        return kIOReturnSuccess;

    for(uint64_t waited = 0; waited < timeout_us; waited += DefaultPollUs) {
        result = spm_->delay(DefaultPollUs);
        if(result != kIOReturnSuccess)return result;
        result = readWord(spm_, offset, &value);
        if(result != kIOReturnSuccess)return result;
        if(set ? ((value & mask) == mask) : ((value & mask) == 0))
            return kIOReturnSuccess;
    }
    return kIOReturnTimeout;
}

IOReturn NativeMaliPower::releaseMfgBusProtect(uint64_t timeout_us) const
{
    /* clk-mt6768-pg.c:1567-1584 (STA_POWER_ON branch of mfg_bus_prot). */
    IOReturn result = modify(spm_, SpmMfgPowerCon, PowerConSramPowerDown, 0);
    if(result != kIOReturnSuccess)return result;
    result = waitControl(SpmMfgPowerCon, PowerConSramPowerDownAck, false,
        timeout_us);
    if(result != kIOReturnSuccess)return result;
    result = writeWord(infracfg_, InfraProtectClear, MfgProtectStep2);
    if(result != kIOReturnSuccess)return result;
    return writeWord(infracfg_, InfraProtectClear, MfgProtectStep1);
}

IOReturn NativeMaliPower::assertMfgBusProtect(uint64_t timeout_us) const
{
    /* clk-mt6768-pg.c:1536-1563 (STA_POWER_DOWN branch). */
    IOReturn result = writeWord(infracfg_, InfraProtectSet, MfgProtectStep1);
    if(result != kIOReturnSuccess)return result;
    result = waitFor(infracfg_, InfraProtectStatus1, MfgProtectStep1,
        timeout_us, DefaultPollUs, nullptr);
    if(result != kIOReturnSuccess)return result;
    result = writeWord(infracfg_, InfraProtectSet, MfgProtectStep2);
    if(result != kIOReturnSuccess)return result;
    result = waitFor(infracfg_, InfraProtectStatus1, MfgProtectStep2,
        timeout_us, DefaultPollUs, nullptr);
    if(result != kIOReturnSuccess)return result;
    result = modify(spm_, SpmMfgPowerCon, 0, PowerConSramPowerDown);
    if(result != kIOReturnSuccess)return result;
    return waitControl(SpmMfgPowerCon, PowerConSramPowerDownAck, true,
        timeout_us);
}

IOReturn NativeMaliPower::powerOn(const Domain &domain,
    uint64_t timeout_us) const
{
    uint32_t status = 0;
    uint32_t status2 = 0;
    IOReturn result = readWord(spm_, SpmPowerStatus, &status);
    if(result != kIOReturnSuccess)return result;
    result = readWord(spm_, SpmPowerStatus2, &status2);
    if(result != kIOReturnSuccess)return result;
    const bool already_on = (status & domain.status) &&
        (status2 & domain.status);

    if(!already_on) {
        result = modify(spm_, domain.control, 0,
            PowerConOn | PowerConOnSecond);
        if(result != kIOReturnSuccess)return result;
        result = waitStatus(domain.status, timeout_us);
        if(result != kIOReturnSuccess) {
            powerOff(domain, timeout_us);
            return result;
        }
    }

    result = modify(spm_, domain.control, PowerConClockDisable, 0);
    if(result != kIOReturnSuccess) {
        powerOff(domain, timeout_us);
        return result;
    }
    result = modify(spm_, domain.control, PowerConIso, 0);
    if(result != kIOReturnSuccess) {
        powerOff(domain, timeout_us);
        return result;
    }
    result = modify(spm_, domain.control, 0, PowerConResetB);
    if(result != kIOReturnSuccess) {
        powerOff(domain, timeout_us);
        return result;
    }
    /* Vendor order: power, de-gate, then bus-protect release (pg_prepare line
     * 3242 before 3269). The release also clears SRAM_PDN and waits for the
     * ack, which is why the MFG domain does not run the generic SRAM step. */
    if(domain.bus_protect) {
        result = releaseMfgBusProtect(timeout_us);
        if(result != kIOReturnSuccess) {
            powerOff(domain, timeout_us);
            return result;
        }
    }
    if(domain.sram_power_down) {
        result = modify(spm_, domain.control, PowerConSramPowerDown, 0);
        if(result != kIOReturnSuccess) {
            powerOff(domain, timeout_us);
            return result;
        }
        result = waitControl(domain.control, PowerConSramPowerDownAck,
            false, timeout_us);
        if(result != kIOReturnSuccess) {
            powerOff(domain, timeout_us);
            return result;
        }
    }
    return kIOReturnSuccess;
}

IOReturn NativeMaliPower::powerOff(const Domain &domain,
    uint64_t timeout_us) const
{
    IOReturn result = kIOReturnSuccess;

    /* Vendor order: MFG asserts bus protection (and powers the SRAM down)
     * before the domain goes away (pg_unprepare line 3295 before 3319); core
     * domains gate their SRAM first and drop power afterwards
     * (clk-mt6768-pg.c:1907-1932). */
    if(domain.bus_protect) {
        result = assertMfgBusProtect(timeout_us);
        if(result != kIOReturnSuccess)return result;
    } else if(domain.sram_power_down) {
        result = modify(spm_, domain.control, 0, PowerConSramPowerDown);
        if(result != kIOReturnSuccess)return result;
        result = waitControl(domain.control, PowerConSramPowerDownAck, true,
            timeout_us);
        if(result != kIOReturnSuccess)return result;
    }
    result = modify(spm_, domain.control, 0, PowerConIso);
    if(result != kIOReturnSuccess)return result;
    result = modify(spm_, domain.control, 0, PowerConClockDisable);
    if(result != kIOReturnSuccess)return result;
    result = modify(spm_, domain.control, PowerConResetB, 0);
    if(result != kIOReturnSuccess)return result;
    result = modify(spm_, domain.control, PowerConOn | PowerConOnSecond, 0);
    if(result != kIOReturnSuccess)return result;
    return waitStatusOff(domain.status, timeout_us);
}

IOReturn NativeMaliPower::probe(NativeMaliPowerObservation *observation)
{
    if(!observation)return kIOReturnBadArgument;
    *observation = NativeMaliPowerObservation{};
    if(!prepared())return kIOReturnNotReady;

    IOReturn result = readWord(spm_, SpmPowerStatus, &observation->spmStatus);
    if(result != kIOReturnSuccess)return result;
    result = readWord(spm_, SpmPowerStatus2, &observation->spmStatus2);
    if(result != kIOReturnSuccess)return result;
    result = readWord(infracfg_, InfraProtectEnable,
        &observation->infraProtectEnable);
    if(result != kIOReturnSuccess)return result;
    result = readWord(infracfg_, InfraProtectStatus1,
        &observation->infraProtectStatus1);
    if(result != kIOReturnSuccess)return result;
    result = readWord(topckgen_, TopMfgClockConfig,
        &observation->topMfgClockConfig);
    if(result != kIOReturnSuccess)return result;
    result = readWord(topckgen_, TopClockUpdate,
        &observation->topClockUpdate);
    if(result != kIOReturnSuccess)return result;
    result = readWord(mfgcfg_, MfgClockGateStatus,
        &observation->mfgClockGateStatus);
    if(result != kIOReturnSuccess)return result;
    result = pmic_.readVgpu(&observation->vgpu);
    if(result != kIOReturnSuccess)return result;
    return pmic_.readVsram(&observation->vsram);
}

IOReturn NativeMaliPower::gpuReadable(uint32_t *missing) const
{
    if(missing)*missing = 0;
    if(!prepared())return kIOReturnNotReady;

    uint32_t status = 0;
    uint32_t status2 = 0;
    IOReturn result = readWord(spm_, SpmPowerStatus, &status);
    if(result != kIOReturnSuccess)return result;
    result = readWord(spm_, SpmPowerStatus2, &status2);
    if(result != kIOReturnSuccess)return result;
    const uint32_t domains = SpmPowerStatusMfg | SpmPowerStatusMfgAsync |
        SpmPowerStatusMfgCore0 | SpmPowerStatusMfgCore1;

    uint32_t top = 0;
    result = readWord(topckgen_, TopMfgClockConfig, &top);
    if(result != kIOReturnSuccess)return result;

    uint32_t gate = 0;
    result = readWord(mfgcfg_, MfgClockGateStatus, &gate);
    if(result != kIOReturnSuccess)return result;

    uint32_t absent = 0;
    if((status & domains) != domains || (status2 & domains) != domains)
        absent |= GpuReadMissingDomains;
    if(top & TopMfgMuxGate)absent |= GpuReadMissingClockMux;
    if(gate & MfgClockGateBg3d)absent |= GpuReadMissingClockGate;

    if(missing)*missing = absent;
    return absent ? kIOReturnNotReady : kIOReturnSuccess;
}

IOReturn NativeMaliPower::setFixedVoltage()
{
    if(!prepared())return kIOReturnNotReady;
    if(!kWriteAuthorized)return kIOReturnUnsupported;
    return pmic_.setFixed();
}

IOReturn NativeMaliPower::selectMfgClockSource()
{
    if(!prepared())return kIOReturnNotReady;
    if(!kWriteAuthorized)return kIOReturnUnsupported;

    const uint32_t field = TopMfgSelectMask << TopMfgSelectShift;
    const uint32_t select = TopMfgSelectClk26m << TopMfgSelectShift;

    /* clk-mux.c:82-100 (enable: clear the mux gate) and 205-228 (set parent:
     * mask to CLR, value to SET, then pulse the update bit). */
    IOReturn result = writeWord(topckgen_, TopMfgClockConfigClear,
        TopMfgMuxGate);
    if(result != kIOReturnSuccess)return result;
    result = writeWord(topckgen_, TopMfgClockConfigClear, field);
    if(result != kIOReturnSuccess)return result;
    result = writeWord(topckgen_, TopMfgClockConfigSet, select);
    if(result != kIOReturnSuccess)return result;
    result = writeWord(topckgen_, TopClockUpdate, TopMfgUpdate);
    if(result != kIOReturnSuccess)return result;

    /* Do not verify the update bit: the vendor never reads it and it may be
     * self-clearing. The mux field and the gate are latched and readable. */
    uint32_t value = 0;
    result = readWord(topckgen_, TopMfgClockConfig, &value);
    if(result != kIOReturnSuccess)return result;
    if(((value >> TopMfgSelectShift) & TopMfgSelectMask) != TopMfgSelectClk26m)
        return kIOReturnIOError;
    if(value & TopMfgMuxGate)return kIOReturnIOError;
    return kIOReturnSuccess;
}

IOReturn NativeMaliPower::enableMfgClockGate()
{
    if(!prepared())return kIOReturnNotReady;
    if(!kWriteAuthorized)return kIOReturnUnsupported;

    uint32_t status = 0;
    IOReturn result = readWord(mfgcfg_, MfgClockGateStatus, &status);
    if(result != kIOReturnSuccess)return result;
    if(status & MfgClockGateBg3d) {
        result = writeWord(mfgcfg_, MfgClockGateClear, MfgClockGateBg3d);
        if(result != kIOReturnSuccess)return result;
    }
    result = readWord(mfgcfg_, MfgClockGateStatus, &status);
    if(result != kIOReturnSuccess)return result;
    /* clk-gate.c:167-175: for set/clr gates, a cleared status bit means
     * enabled. */
    if(status & MfgClockGateBg3d)return kIOReturnIOError;
    return kIOReturnSuccess;
}

IOReturn NativeMaliPower::disableMfgClockGate()
{
    if(!prepared())return kIOReturnNotReady;
    if(!kWriteAuthorized)return kIOReturnUnsupported;

    IOReturn result = writeWord(mfgcfg_, MfgClockGateSet, MfgClockGateBg3d);
    if(result != kIOReturnSuccess)return result;
    uint32_t status = 0;
    result = readWord(mfgcfg_, MfgClockGateStatus, &status);
    if(result != kIOReturnSuccess)return result;
    /* clk-gate.c:167-175: for set/clr gates a set status bit means gated. */
    if(!(status & MfgClockGateBg3d))return kIOReturnIOError;
    return kIOReturnSuccess;
}

IOReturn NativeMaliPower::powerOnDomains(uint64_t timeout_us)
{
    if(!prepared())return kIOReturnNotReady;
    if(!kWriteAuthorized)return kIOReturnUnsupported;

    const Domain *domains = mfgDomains();

    bool up[kDomainCount]{};
    for(uint32_t index = 0; index < kDomainCount; ++index) {
        const IOReturn result = powerOn(domains[index], timeout_us);
        if(result != kIOReturnSuccess) {
            /* The failing domain has already put itself back; undo only the
             * ones that reported success, newest first. */
            for(uint32_t undo = index; undo > 0; --undo) {
                if(up[undo - 1])powerOff(domains[undo - 1], timeout_us);
            }
            return result;
        }
        up[index] = true;
    }
    return kIOReturnSuccess;
}

IOReturn NativeMaliPower::powerDownDomains(uint64_t timeout_us)
{
    if(!prepared())return kIOReturnNotReady;
    if(!kWriteAuthorized)return kIOReturnUnsupported;

    const Domain *domains = mfgDomains();
    IOReturn first = kIOReturnSuccess;
    for(uint32_t index = kDomainCount; index > 0; --index) {
        const IOReturn result = powerOff(domains[index - 1], timeout_us);
        if(result != kIOReturnSuccess && first == kIOReturnSuccess)first = result;
    }
    return first;
}

IOReturn NativeMaliPower::tearDown(uint64_t timeout_us)
{
    if(!prepared())return kIOReturnNotReady;
    if(!kWriteAuthorized)return kIOReturnUnsupported;

    /* Vendor order: clock gate first, then the domains (mt_gpufreq_disable_CG()
     * before mt_gpufreq_disable_MTCMOS() in the power-off path that
     * mt_gpufreq_power_disable() builds). Both halves run even if the first
     * fails, and the first error is the one returned. */
    const IOReturn gate = disableMfgClockGate();
    const IOReturn domains = powerDownDomains(timeout_us);
    if(gate != kIOReturnSuccess)return gate;
    return domains;
}

IOReturn NativeMaliPower::bringUp(uint64_t timeout_us)
{
    if(!prepared())return kIOReturnNotReady;
    if(!kWriteAuthorized)return kIOReturnUnsupported;

    IOReturn result = setFixedVoltage();
    if(result != kIOReturnSuccess)return result;
    result = selectMfgClockSource();
    if(result != kIOReturnSuccess)return result;
    result = powerOnDomains(timeout_us);
    if(result != kIOReturnSuccess)return result;
    result = enableMfgClockGate();
    if(result != kIOReturnSuccess) {
        /* Quiesce: the domains came up but the gate did not open, so the GPU
         * cannot be used. Leave the rails alone, take the domains down. The
         * gate error is the one that matters, so the power-down result is not
         * returned; powerDownDomains() is idempotent for a domain already off,
         * so a caller can simply call tearDown() again. */
        powerDownDomains(timeout_us);
        return result;
    }
    return kIOReturnSuccess;
}
