/* SPDX-License-Identifier: GPL-2.0-only */
/* MT6358 GPU rails; every address and its citation is in NativeMaliMt6358.h. */
#include "NativeMaliMt6358.h"

IOReturn NativeMaliMt6358::prepare(NativeMaliPwrap *pwrap)
{
    if(!pwrap || !pwrap->prepared())return kIOReturnBadArgument;
    if(pwrap_ && pwrap_ != pwrap)return kIOReturnBusy;
    pwrap_ = pwrap;
    return kIOReturnSuccess;
}

bool NativeMaliMt6358::encode(uint32_t microvolts, uint16_t *selector)
{
    if(!selector)return false;
    *selector = 0;
    if(microvolts < MinimumMicrovolts || microvolts > MaximumMicrovolts)return false;
    const uint32_t offset = microvolts - MinimumMicrovolts;
    if(offset % StepMicrovolts)return false;
    const uint32_t value = offset / StepMicrovolts;
    if(value > VoselMask)return false;
    *selector = static_cast<uint16_t>(value);
    return true;
}

bool NativeMaliMt6358::decode(uint16_t selector, uint32_t *microvolts)
{
    if(!microvolts)return false;
    *microvolts = 0;
    if(selector > VoselMask)return false;
    *microvolts = MinimumMicrovolts +
        static_cast<uint32_t>(selector) * StepMicrovolts;
    return true;
}

IOReturn NativeMaliMt6358::readRail(uint16_t enable_offset,
    uint16_t vosel_offset, RailState *state)
{
    if(!state)return kIOReturnBadArgument;
    *state = RailState{};
    if(!pwrap_)return kIOReturnNotReady;

    IOReturn result = pwrap_->read(enable_offset, &state->enableWord);
    if(result != kIOReturnSuccess)return result;
    result = pwrap_->read(vosel_offset, &state->voselWord);
    if(result != kIOReturnSuccess)return result;

    state->enabled = (state->enableWord & EnableMask) != 0;
    if(!decode(state->voselWord & VoselMask, &state->microvolts))
        return kIOReturnIOError;
    return kIOReturnSuccess;
}

IOReturn NativeMaliMt6358::restoreRail(uint16_t enable_offset,
    uint16_t vosel_offset, const RailState &state)
{
    if(!pwrap_)return kIOReturnNotReady;
    /* Voltage first, then enable, so the rail is never enabled at a value it
     * was not at before. Keep going after a failure: a partial restore is
     * better than none, and the caller still returns the original error. */
    IOReturn first = pwrap_->write(vosel_offset, state.voselWord);
    const IOReturn enable = pwrap_->write(enable_offset, state.enableWord);
    if(first == kIOReturnSuccess)first = enable;
    return first;
}

IOReturn NativeMaliMt6358::setRail(uint16_t enable_offset,
    uint16_t vosel_offset, uint32_t microvolts, RailState *before)
{
    if(!pwrap_)return kIOReturnNotReady;
    uint16_t selector = 0;
    if(!encode(microvolts, &selector))return kIOReturnBadArgument;

    RailState previous;
    IOReturn result = readRail(enable_offset, vosel_offset, &previous);
    if(result != kIOReturnSuccess)return result;
    if(before)*before = previous;

    if((previous.voselWord & VoselMask) != selector) {
        const uint16_t next = static_cast<uint16_t>(
            (previous.voselWord & ~VoselMask) | selector);
        result = pwrap_->write(vosel_offset, next);
        if(result != kIOReturnSuccess)return result;
        uint16_t readback = 0;
        result = pwrap_->read(vosel_offset, &readback);
        if(result != kIOReturnSuccess)return result;
        if((readback & VoselMask) != selector) {
            /* Put the old word back; the rail keeps its previous voltage. */
            pwrap_->write(vosel_offset, previous.voselWord);
            return kIOReturnIOError;
        }
    }

    if(!previous.enabled) {
        const uint16_t next = static_cast<uint16_t>(
            previous.enableWord | EnableMask);
        result = pwrap_->write(enable_offset, next);
        if(result != kIOReturnSuccess)return result;
        uint16_t readback = 0;
        result = pwrap_->read(enable_offset, &readback);
        if(result != kIOReturnSuccess)return result;
        if((readback & EnableMask) == 0) {
            pwrap_->write(enable_offset, previous.enableWord);
            return kIOReturnIOError;
        }
    }
    return kIOReturnSuccess;
}

IOReturn NativeMaliMt6358::readVgpu(RailState *state)
{
    return readRail(VgpuEnable, VgpuVosel, state);
}

IOReturn NativeMaliMt6358::readVsram(RailState *state)
{
    return readRail(VsramEnable, VsramVosel, state);
}

IOReturn NativeMaliMt6358::setVgpu(uint32_t microvolts, RailState *before)
{
    return setRail(VgpuEnable, VgpuVosel, microvolts, before);
}

IOReturn NativeMaliMt6358::setVsram(uint32_t microvolts, RailState *before)
{
    return setRail(VsramEnable, VsramVosel, microvolts, before);
}

IOReturn NativeMaliMt6358::setFixed()
{
    if(!pwrap_)return kIOReturnNotReady;

    RailState vsram_before;
    RailState vgpu_before;
    IOReturn result = readVsram(&vsram_before);
    if(result != kIOReturnSuccess)return result;
    result = readVgpu(&vgpu_before);
    if(result != kIOReturnSuccess)return result;

    result = setVsram(FixedVsramMicrovolts);
    if(result != kIOReturnSuccess)return result;

    result = setVgpu(FixedVgpuMicrovolts);
    if(result != kIOReturnSuccess) {
        restoreRail(VsramEnable, VsramVosel, vsram_before);
        return result;
    }
    return kIOReturnSuccess;
}
