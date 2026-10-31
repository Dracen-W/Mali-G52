/* SPDX-License-Identifier: GPL-2.0-only */
/* WACS2 transport; the protocol and its citations are in NativeMaliPwrap.h. */
#include "NativeMaliPwrap.h"

IOReturn NativeMaliPwrap::prepare(NativeMaliRegisterTarget *target)
{
    if(!target)return kIOReturnBadArgument;
    if(target_ && target_ != target)return kIOReturnBusy;
    target_ = target;
    return kIOReturnSuccess;
}

IOReturn NativeMaliPwrap::waitForFsm(uint32_t wanted, bool clear_valid,
    uint32_t *word, uint64_t timeout_us)
{
    if(!target_)return kIOReturnNotReady;
    const uint32_t poll_us = DefaultPollUs;
    uint32_t value = 0;
    uint64_t waited = 0;
    for(;;) {
        IOReturn result = target_->read32(Wacs2Data, &value);
        if(result != kIOReturnSuccess)return result;
        /* pwrap_hal.c:390-410 refuses to touch the FSM until the wrapper says
         * initialisation finished. There is no safe recovery from "not yet",
         * so this is reported rather than waited out. */
        if((value & InitDone) == 0)return kIOReturnNotReady;
        const uint32_t fsm = (value >> FsmShift) & FsmMask;
        if(fsm == wanted) {
            if(word)*word = value;
            return kIOReturnSuccess;
        }
        if(clear_valid && fsm == FsmWaitValidClear) {
            /* pwrap_hal.c:399-401: acknowledge the stale read before retrying. */
            result = target_->write32(Wacs2Clear, 1);
            if(result != kIOReturnSuccess)return result;
        }
        if(waited >= timeout_us) {
            if(word)*word = value;
            return kIOReturnTimeout;
        }
        result = target_->delay(poll_us);
        if(result != kIOReturnSuccess)return result;
        waited += poll_us;
    }
}

IOReturn NativeMaliPwrap::waitIdle(uint64_t timeout_us)
{
    return waitForFsm(FsmIdle, true, nullptr, timeout_us);
}

IOReturn NativeMaliPwrap::read(uint16_t address, uint16_t *value,
    uint64_t timeout_us)
{
    if(!value)return kIOReturnBadArgument;
    *value = 0;
    if(!target_)return kIOReturnNotReady;
    if(!timeout_us || !DefaultPollUs)return kIOReturnBadArgument;

    IOReturn result = waitIdle(timeout_us);
    if(result != kIOReturnSuccess)return result;

    const uint32_t command =
        (static_cast<uint32_t>(address) >> 1) << AddressShift;
    result = target_->write32(Wacs2Command, command);
    if(result != kIOReturnSuccess)return result;

    uint32_t word = 0;
    result = waitForFsm(FsmWaitValidClear, false, &word, timeout_us);
    if(result != kIOReturnSuccess)return result;
    *value = static_cast<uint16_t>(word & DataMask);

    /* pwrap_hal.c:494-495: reading the data is what clears the valid bit. */
    result = target_->write32(Wacs2Clear, 1);
    if(result != kIOReturnSuccess)return result;
    return waitIdle(timeout_us);
}

IOReturn NativeMaliPwrap::write(uint16_t address, uint16_t value,
    uint64_t timeout_us)
{
    if(!target_)return kIOReturnNotReady;
    if(!timeout_us || !DefaultPollUs)return kIOReturnBadArgument;

    IOReturn result = waitIdle(timeout_us);
    if(result != kIOReturnSuccess)return result;

    const uint32_t command = CommandWrite |
        ((static_cast<uint32_t>(address) >> 1) << AddressShift) |
        static_cast<uint32_t>(value);
    result = target_->write32(Wacs2Command, command);
    if(result != kIOReturnSuccess)return result;
    return waitIdle(timeout_us);
}
