/* SPDX-License-Identifier: GPL-2.0-only */
/* Fake register blocks for the M2/M3 host tests.
 *
 * These are deliberately small models, not emulators: enough behaviour for the
 * driver's own sequence to run (a write-1-to-clear bank, a domain status that
 * follows PWR_ON with a configurable delay, a WACS2 wrapper that speaks the
 * real command/FSM protocol). Everything they do not model is a plain word.
 *
 * `memory` offsets are bytes, as in the driver. `pollsBeforeUp` is how many
 * status reads a domain stays invisibly off after PWR_ON, which is what the
 * polling and timeout tests need.
 */
#ifndef NATIVE_MALI_TEST_FAKE_PLATFORM_H
#define NATIVE_MALI_TEST_FAKE_PLATFORM_H
#include "NativeMaliPlatform.h"
#include "NativeMaliRegs.h"
#include <cstring>
#include <map>
#include <stdint.h>
#include <vector>

class FakeRegisterBlock : public NativeMaliRegisterTarget {
public:
    /* The mali node's reg size from the device tree (tests/data/mt6768.dtb):
     * the GPU core, job slots and MMU (0x2400) all live in this window.
     * Heap-backed on purpose: every fake block is 16 KiB, a test fixture holds
     * several of them, and a few dozen fixtures in one inlined function overflow
     * the 8 MiB stack under AddressSanitizer. */
    enum : uint32_t { Window = 0x4000, Words = Window / 4 };
    std::vector<uint32_t> memory = std::vector<uint32_t>(Words);
    struct Write { uint32_t offset; uint32_t value; uint32_t seq = 0; };
    std::vector<Write> writes;
    /* One counter shared by every block of a rig, so a test can prove
     * cross-block order ("did the clock gate open before the first GPU write?")
     * instead of only the order inside one block. Zero when no test wired one
     * up; the per-block order is then still the vector index. */
    uint32_t *sequence = nullptr;
    uint32_t readCount = 0;
    std::vector<uint64_t> delays;
    int32_t failReadAt = -1;
    int32_t failWriteAt = -1;
    IOReturn failReadResult = kIOReturnIOError;
    IOReturn failWriteResult = kIOReturnIOError;
    IOReturn delayResult = kIOReturnSuccess;

    uint32_t nextSequence()
    {
        return sequence ? ++*sequence : 0;
    }

    /* Every write goes through here: the vector is the per-block order, `seq`
     * is the rig-wide one. */
    void record(uint32_t offset, uint32_t value)
    {
        writes.push_back({offset, value, nextSequence()});
    }

    static bool valid(uint32_t offset)
    {
        return (offset % 4) == 0 && offset / 4 < Words;
    }

    /* True when `offset` is job-manager field `field` (a JS_* offset relative to
     * JS_BASE, e.g. JS_COMMAND(0) - JS_BASE) of some slot, with the slot index
     * in *slot. The slots sit between JS_BASE and MMU_BASE. */
    static bool jobSlotField(uint32_t offset, uint32_t field, uint32_t *slot)
    {
        if(offset < JS_BASE || offset >= MMU_BASE)return false;
        const uint32_t relative = offset - JS_BASE;
        if(relative % JS_SLOT_STRIDE != field)return false;
        if(slot)*slot = relative / JS_SLOT_STRIDE;
        return true;
    }

    IOReturn read32(uint32_t offset, uint32_t *value) override
    {
        ++readCount;
        if(failReadAt >= 0 && offset == static_cast<uint32_t>(failReadAt))
            return failReadResult;
        if(!value || !valid(offset))return kIOReturnBadArgument;
        *value = memory[offset / 4];
        return kIOReturnSuccess;
    }

    IOReturn write32(uint32_t offset, uint32_t value) override
    {
        record(offset, value);
        if(failWriteAt >= 0 && offset == static_cast<uint32_t>(failWriteAt))
            return failWriteResult;
        if(!valid(offset))return kIOReturnBadArgument;
        memory[offset / 4] = value;
        return kIOReturnSuccess;
    }

    IOReturn delay(uint64_t microseconds) override
    {
        delays.push_back(microseconds);
        return delayResult;
    }

    size_t countWrites(uint32_t offset) const
    {
        size_t count = 0;
        for(const Write &write : writes)
            if(write.offset == offset)++count;
        return count;
    }

    int32_t indexOfWrite(uint32_t offset, uint32_t value) const
    {
        for(size_t index = 0; index < writes.size(); ++index)
            if(writes[index].offset == offset && writes[index].value == value)
                return static_cast<int32_t>(index);
        return -1;
    }
};

/* SPM. Status is derived from the PWR_ON/PWR_ON_2ND bits the driver wrote, and
 * SRAM_PDN_ACK mirrors SRAM_PDN, which is what the vendor ack loops expect. */
class FakeSpm final : public FakeRegisterBlock {
public:
    /* The two PWR_CON bits the fake has to model: the SRAM power-down request
     * and the ack that follows it (NativeMaliPower::PowerConSramPowerDown /
     * PowerConSramPowerDownAck, from clk-mt6768-pg.c). */
    enum : uint32_t { kSramPowerDown = 1u << 8, kSramPowerDownAck = 1u << 12 };

    int pollsBeforeUp = 0;
    /* A control register whose domain never reports on in PWR_STATUS. */
    uint32_t stuckDomain = 0;
    /* A control register whose status bit stays on even after PWR_ON is
     * cleared, so a power-down can be made to time out. */
    uint32_t stuckOffDomain = 0;
    /* When false, PWR_STATUS/PWR_STATUS_2ND read straight from `memory`, which
     * is what a probe test needs to hand the driver a known raw word. */
    bool deriveStatus = true;

    static bool isDomainControl(uint32_t offset)
    {
        return offset == 0x310 || offset == 0x334 || offset == 0x338 ||
            offset == 0x34C;
    }

    static uint32_t statusBit(uint32_t offset)
    {
        switch(offset) {
        case 0x310: return 1u << 13;
        case 0x334: return 1u << 14;
        case 0x338: return 1u << 11;
        case 0x34C: return 1u << 12;
        default: return 0;
        }
    }

    IOReturn write32(uint32_t offset, uint32_t value) override
    {
        /* SRAM_PDN_ACK is derived on read (below). A read-modify-write follows
         * the fake's own read, so without masking it here the driver would
         * latch the ack into the control word and the next power-up would see
         * an ack that never clears. Real hardware ignores writes to the ack
         * bit; this keeps the model honest about that. */
        if(isDomainControl(offset))value &= ~kSramPowerDownAck;
        const bool was_on = isDomainControl(offset) &&
            (memory[offset / 4] & 0xC) == 0xC;
        const bool now_on = isDomainControl(offset) && (value & 0xC) == 0xC;
        IOReturn result = FakeRegisterBlock::write32(offset, value);
        if(result == kIOReturnSuccess && !was_on && now_on &&
            pollsBeforeUp > 0) {
            /* PWR_STATUS and PWR_STATUS_2ND each take their own `pollsBeforeUp`
             * reads to catch up, because the driver waits for both. */
            pending[key(offset, 0x180)] = pollsBeforeUp;
            pending[key(offset, 0x184)] = pollsBeforeUp;
        }
        return result;
    }

    IOReturn read32(uint32_t offset, uint32_t *value) override
    {
        if(failReadAt >= 0 && offset == static_cast<uint32_t>(failReadAt))
            return failReadResult;
        if(value && (offset == 0x310 || offset == 0x334 || offset == 0x338 ||
            offset == 0x34C)) {
            ++readCount;
            uint32_t word = memory[offset / 4];
            if(word & kSramPowerDown)word |= kSramPowerDownAck;
            *value = word;
            return kIOReturnSuccess;
        }
        if(value && (offset == 0x180 || offset == 0x184)) {
            if(!deriveStatus)return FakeRegisterBlock::read32(offset, value);
            ++readCount;
            uint32_t bits = 0;
            const uint32_t controls[] = {0x334, 0x338, 0x34C, 0x310};
            for(uint32_t control : controls) {
                if(control == stuckOffDomain) {
                    bits |= statusBit(control);
                    continue;
                }
                if(control == stuckDomain)continue;
                if((memory[control / 4] & 0xC) != 0xC)continue;
                auto pendingPolls = pending.find(key(control, offset));
                if(pendingPolls != pending.end() && pendingPolls->second > 0) {
                    --pendingPolls->second;
                    continue;
                }
                bits |= statusBit(control);
            }
            *value = bits;
            return kIOReturnSuccess;
        }
        return FakeRegisterBlock::read32(offset, value);
    }

    /* Reset verdicts and delays between phases of a test. */
    void clearPending()
    {
        pending.clear();
        pollsBeforeUp = 0;
    }

private:
    static uint32_t key(uint32_t control, uint32_t status_offset)
    {
        return (control << 1) | (status_offset == 0x184 ? 1u : 0u);
    }

    std::map<uint32_t, int> pending;
};

/* INFRACFG_AO: PROTECTEN_SET/CLR update the PROTECTEN word, and STA1 reads it
 * back, so the vendor ack polls have something sane to observe. */
class FakeInfra final : public FakeRegisterBlock {
public:
    /* Set to another block's word to see what it held when bus protection was
     * changed; that is how the tests pin the vendor's ordering. */
    uint32_t *watchWord = nullptr;
    bool sawProtectSet = false;
    uint32_t watchAtProtectSet = 0;
    bool sawProtectClear = false;
    uint32_t watchAtProtectClear = 0;

    IOReturn write32(uint32_t offset, uint32_t value) override
    {
        if(offset == 0x2A0) {
            if(watchWord) {
                watchAtProtectSet = *watchWord;
                sawProtectSet = true;
            }
            record(offset, value);
            if(failWriteAt >= 0 && offset == static_cast<uint32_t>(failWriteAt))
                return failWriteResult;
            memory[0x220 / 4] |= value;
            return kIOReturnSuccess;
        }
        if(offset == 0x2A4) {
            if(watchWord) {
                watchAtProtectClear = *watchWord;
                sawProtectClear = true;
            }
            record(offset, value);
            if(failWriteAt >= 0 && offset == static_cast<uint32_t>(failWriteAt))
                return failWriteResult;
            memory[0x220 / 4] &= ~value;
            return kIOReturnSuccess;
        }
        return FakeRegisterBlock::write32(offset, value);
    }

    IOReturn read32(uint32_t offset, uint32_t *value) override
    {
        if(offset == 0x228) {
            ++readCount;
            if(!value)return kIOReturnBadArgument;
            *value = memory[0x220 / 4];
            return kIOReturnSuccess;
        }
        return FakeRegisterBlock::read32(offset, value);
    }
};

/* TOPCKGEN: the CLR/SET registers clear/set bits in the matching CLK_CFG word.
 * Only CLK_CFG_1 is modelled; everything else is plain. */
class FakeTopckgen final : public FakeRegisterBlock {
public:
    IOReturn write32(uint32_t offset, uint32_t value) override
    {
        if(offset == 0x58) {
            record(offset, value);
            if(failWriteAt >= 0 && offset == static_cast<uint32_t>(failWriteAt))
                return failWriteResult;
            memory[0x50 / 4] &= ~value;
            return kIOReturnSuccess;
        }
        if(offset == 0x54) {
            record(offset, value);
            if(failWriteAt >= 0 && offset == static_cast<uint32_t>(failWriteAt))
                return failWriteResult;
            memory[0x50 / 4] |= value;
            return kIOReturnSuccess;
        }
        return FakeRegisterBlock::write32(offset, value);
    }
};

/* MFG_CFG: gate status bit 0; write 1 to CLR and it reads 0. */
class FakeMfgcfg final : public FakeRegisterBlock {
public:
    bool ignoreClear = false;
    bool ignoreSet = false;

    IOReturn write32(uint32_t offset, uint32_t value) override
    {
        if(offset == 0x8) {
            record(offset, value);
            if(failWriteAt >= 0 && offset == static_cast<uint32_t>(failWriteAt))
                return failWriteResult;
            if(!ignoreClear)memory[0] &= ~value;
            return kIOReturnSuccess;
        }
        if(offset == 0x4) {
            record(offset, value);
            if(failWriteAt >= 0 && offset == static_cast<uint32_t>(failWriteAt))
                return failWriteResult;
            if(!ignoreSet)memory[0] |= value;
            return kIOReturnSuccess;
        }
        return FakeRegisterBlock::write32(offset, value);
    }
};

/* WACS2 wrapper plus the 16-bit PMIC register file behind it. */
class FakePwrap final : public FakeRegisterBlock {
public:
    uint16_t pmic[0x10000]{};
    bool initDone = true;
    bool neverReady = false;
    int32_t dropPmicWriteAt = -1;   /* returns success, keeps the old value */

    void setPmic(uint16_t address, uint16_t value) { pmic[address] = value; }
    uint16_t getPmic(uint16_t address) const { return pmic[address]; }
    /* Leave a completed read unacknowledged, which is the stale-WFVLDCLR case
     * the vendor's idle wait clears before starting a new command. */
    void makeStale(uint16_t address, uint16_t value)
    {
        commandAddress = address;
        pmic[address] = value;
        state = Ready;
    }

    IOReturn write32(uint32_t offset, uint32_t value) override
    {
        record(offset, value);
        if(failWriteAt >= 0 && offset == static_cast<uint32_t>(failWriteAt))
            return failWriteResult;
        if(offset == 0xC20) {
            const uint16_t address = static_cast<uint16_t>(
                ((value >> 16) & 0xFFFF) << 1);
            commandAddress = address;
            if(value & 0x80000000u) {
                if(dropPmicWriteAt < 0 ||
                    address != static_cast<uint16_t>(dropPmicWriteAt))
                    pmic[address] = static_cast<uint16_t>(value & 0xFFFF);
                state = Idle;
            } else {
                state = Ready;
            }
            return kIOReturnSuccess;
        }
        if(offset == 0xC28) {
            state = Idle;
            return kIOReturnSuccess;
        }
        if(!valid(offset))return kIOReturnBadArgument;
        memory[offset / 4] = value;
        return kIOReturnSuccess;
    }

    IOReturn read32(uint32_t offset, uint32_t *value) override
    {
        ++readCount;
        if(failReadAt >= 0 && offset == static_cast<uint32_t>(failReadAt))
            return failReadResult;
        if(!value)return kIOReturnBadArgument;
        if(offset == 0xC24) {
            if(!initDone) {
                *value = 0;
                return kIOReturnSuccess;
            }
            uint32_t word = 1u << 21;
            if(state == Ready && !neverReady)
                word |= (6u << 16) |
                    (static_cast<uint32_t>(pmic[commandAddress]) & 0xFFFF);
            *value = word;
            return kIOReturnSuccess;
        }
        if(!valid(offset))return kIOReturnBadArgument;
        *value = memory[offset / 4];
        return kIOReturnSuccess;
    }

private:
    enum : int { Idle = 0, Ready = 1 };
    int state = Idle;
    uint16_t commandAddress = 0;
};

/* GPU control registers. The reset handshake and the power/ready handshake are
 * modelled: INT_CLEAR clears RAWSTAT bits, GPU_CMD starts a reset that reports
 * RESET_COMPLETED in RAWSTAT after `pollsBeforeReset` reads, and a PWRON write
 * makes the matching READY register report its mask after `readyPolls` reads.
 * INT_MASK and the PWROFF/PWRTRANS side are plain words with the documented
 * countdown. */
class FakeGpu final : public FakeRegisterBlock {
public:
    enum : uint32_t {
        IntRawStat = 0x20,
        IntClear = 0x24,
        IntMask = 0x28,
        Command = 0x30,
        ResetCompleted = 1u << 8,
        CommandSoftReset = 1,
        CommandHardReset = 2,
        /* The job manager in the same window (panfrost_regs.h:233-256). Only
         * the behaviours the slot state machine depends on are modelled:
         * JS_COMMAND_NEXT is consumed, JOB_INT_CLEAR clears RAWSTAT, and
         * JOB_INT_JS_STATE follows START/SOFT_STOP/reset. */
        JobIntRawStat = 0x1000,
        JobIntClear = 0x1004,
        JobIntJsState = 0x1010
    };

    /* A slot whose JS_STATE bit never clears after SOFT_STOP, as if the job
     * manager had hung. Used to reach quiesce()'s 10 ms timeout. */
    bool softStopStuck = false;

    /* JS_COMMAND_NEXT is consumed by the job manager: panfrost writes START and
     * the next submission's WARN_ON(job_read(JS_COMMAND_NEXT)) expects to read
     * 0 back (panfrost_job.c:217,270). */
    static bool isJobCommandNext(uint32_t offset)
    {
        return offset >= JS_BASE && offset < MMU_BASE &&
            ((offset - JS_BASE) % JS_SLOT_STRIDE) ==
                JS_COMMAND_NEXT(0) - JS_BASE;
    }
    /* RAWSTAT reads that return without the reset bit before it appears. */
    int pollsBeforeReset = 0;
    bool softCompletes = true;
    bool hardCompletes = true;
    uint32_t commandWrites = 0;

    /* Power model: READY reads before the mask appears, PWRTRANS reads before
     * the transition clears, and which block actually completes. */
    uint32_t readyPolls = 0;
    uint32_t transitionPolls = 0;
    bool l2Completes = true;
    bool shaderCompletes = true;
    bool tilerCompletes = true;
    bool transitionsSettle = true;

    IOReturn write32(uint32_t offset, uint32_t value) override
    {
        record(offset, value);
        if(failWriteAt >= 0 && offset == static_cast<uint32_t>(failWriteAt))
            return failWriteResult;
        if(offset == IntClear) {
            memory[IntRawStat / 4] &= ~value;
            return kIOReturnSuccess;
        }
        /* JOB_INT_CLEAR is write-1-to-clear on JOB_INT_RAWSTAT
         * (panfrost_regs.h:233-256; panfrost_job.c:435-438 writes it). */
        if(offset == JobIntClear) {
            memory[JobIntRawStat / 4] &= ~value;
            return kIOReturnSuccess;
        }
        if(offset == Command) {
            ++commandWrites;
            /* A soft or hard reset clears the job manager's slot state. */
            if(value == CommandSoftReset || value == CommandHardReset)
                memory[JobIntJsState / 4] = 0;
            if((value == CommandSoftReset && softCompletes) ||
                (value == CommandHardReset && hardCompletes))
                pending = pollsBeforeReset + 1;
            return kIOReturnSuccess;
        }
        if(!valid(offset))return kIOReturnBadArgument;
        /* JS_STATE bit j is set while slot j runs: START schedules the chain
         * (panfrost_job.c:270) and SOFT_STOP asks it to park (:700). The fake
         * clears the bit at SOFT_STOP unless softStopStuck says otherwise. */
        uint32_t js = 0;
        if(jobSlotField(offset, JS_COMMAND_NEXT(0) - JS_BASE, &js)) {
            if(value == JS_COMMAND_START)memory[JobIntJsState / 4] |= 1u << js;
        } else if(jobSlotField(offset, JS_COMMAND(0) - JS_BASE, &js)) {
            if(value == JS_COMMAND_SOFT_STOP && !softStopStuck)
                memory[JobIntJsState / 4] &= ~(1u << js);
        }
        if(offset == L2_PWRON_LO)
            return startPower(L2_READY_LO, offset, value, l2Completes);
        if(offset == SHADER_PWRON_LO)
            return startPower(SHADER_READY_LO, offset, value, shaderCompletes);
        if(offset == TILER_PWRON_LO)
            return startPower(TILER_READY_LO, offset, value, tilerCompletes);
        if(offset == L2_PWROFF_LO)
            return startPowerOff(L2_READY_LO, L2_PWRTRANS_LO, offset, value);
        if(offset == SHADER_PWROFF_LO)
            return startPowerOff(SHADER_READY_LO, SHADER_PWRTRANS_LO, offset,
                value);
        if(offset == TILER_PWROFF_LO)
            return startPowerOff(TILER_READY_LO, TILER_PWRTRANS_LO, offset,
                value);
        memory[offset / 4] = value;
        return kIOReturnSuccess;
    }

    IOReturn read32(uint32_t offset, uint32_t *value) override
    {
        ++readCount;
        if(failReadAt >= 0 && offset == static_cast<uint32_t>(failReadAt))
            return failReadResult;
        if(!value)return kIOReturnBadArgument;
        if(!valid(offset))return kIOReturnBadArgument;
        if(offset == IntRawStat && pending > 0) {
            if(pending == 1) {
                memory[IntRawStat / 4] |= ResetCompleted;
                pending = 0;
            } else {
                --pending;
            }
        }
        if(isJobCommandNext(offset)) {
            *value = 0;
            return kIOReturnSuccess;
        }
        if(powerActive_) {
            if(!powerOff_ && offset == powerReady_) {
                if(--powerCountdown == 0) {
                    memory[powerReady_ / 4] = powerMask_;
                    powerActive_ = false;
                }
            } else if(powerOff_ && offset == powerTrans_) {
                if(--powerCountdown == 0) {
                    memory[powerTrans_ / 4] = 0;
                    powerActive_ = false;
                }
            }
        }
        *value = memory[offset / 4];
        return kIOReturnSuccess;
    }

private:
    IOReturn startPower(uint32_t ready, uint32_t offset, uint32_t mask,
        bool completes)
    {
        memory[offset / 4] = mask;
        memory[ready / 4] = 0;
        if(!completes) {
            powerActive_ = false;
            return kIOReturnSuccess;
        }
        powerReady_ = ready;
        powerMask_ = mask;
        powerCountdown = static_cast<int>(readyPolls) + 1;
        powerOff_ = false;
        powerActive_ = true;
        return kIOReturnSuccess;
    }

    IOReturn startPowerOff(uint32_t ready, uint32_t trans, uint32_t offset,
        uint32_t mask)
    {
        memory[offset / 4] = mask;
        memory[ready / 4] = 0;
        memory[trans / 4] = mask;
        if(!transitionsSettle) {
            powerActive_ = false;
            return kIOReturnSuccess;
        }
        powerTrans_ = trans;
        powerCountdown = static_cast<int>(transitionPolls) + 1;
        powerOff_ = true;
        powerActive_ = true;
        return kIOReturnSuccess;
    }

    int pending = 0;
    int powerCountdown = 0;
    uint32_t powerReady_ = 0;
    uint32_t powerTrans_ = 0;
    uint32_t powerMask_ = 0;
    bool powerActive_ = false;
    bool powerOff_ = false;
};

/* MMU control registers. AS_STATUS is the only MMU register with behaviour: it
 * reports AS_ACTIVE for `activeReads` reads, then settles to the value in
 * `memory`. AS_COMMAND writes are counted.
 *
 * Two job-manager behaviours are modelled because the driver's state machine
 * depends on them: JS_COMMAND_NEXT clears itself once the command is taken, and
 * JOB_INT_CLEAR is write-1-to-clear on JOB_INT_RAWSTAT. `doneAfterReads` /
 * `doneWord` let a test say "the GPU finishes after N status polls". A third is
 * needed for quiesce(): JOB_INT_JS_STATE bit j follows JS_COMMAND_NEXT = START
 * and JS_COMMAND = SOFT_STOP. The rest is a plain word bank. */
class FakeMmu final : public FakeRegisterBlock {
public:
    enum : uint32_t {
        AsBase = 0x2400,
        AsStride = 0x40,
        CommandOffset = 0x18,
        StatusOffset = 0x28,
        StatusActive = 1u   /* AS_STATUS_AS_ACTIVE */
    };
    uint32_t activeReads = 0;
    uint32_t statusReads = 0;
    uint32_t commandWrites = 0;
    /* Latch `doneWord` into JOB_INT_RAWSTAT on the Nth JOB_INT_RAWSTAT read. */
    uint32_t doneAfterReads = 0;
    uint32_t doneWord = 0;
    /* A slot whose JS_STATE bit never clears after SOFT_STOP, as if the job
     * manager had hung. Used to reach quiesce()'s 10 ms timeout. */
    bool softStopStuck = false;

    static bool isCommand(uint32_t offset)
    {
        return offset >= AsBase &&
            ((offset - AsBase) % AsStride) == CommandOffset;
    }
    static bool isStatus(uint32_t offset)
    {
        return offset >= AsBase &&
            ((offset - AsBase) % AsStride) == StatusOffset;
    }
    /* JS_COMMAND_NEXT is consumed by the job manager: panfrost writes START and
     * the next submission's WARN_ON(job_read(JS_COMMAND_NEXT)) expects to read 0
     * back (panfrost_job.c:217,270). A plain word bank would keep the START and
     * refuse every later submission. */
    static bool isJobCommandNext(uint32_t offset)
    {
        return offset >= JS_BASE && offset < MMU_BASE &&
            ((offset - JS_BASE) % JS_SLOT_STRIDE) == JS_COMMAND_NEXT(0) - JS_BASE;
    }

    IOReturn read32(uint32_t offset, uint32_t *value) override
    {
        ++readCount;
        if(failReadAt >= 0 && offset == static_cast<uint32_t>(failReadAt))
            return failReadResult;
        if(!value || !valid(offset))return kIOReturnBadArgument;
        if(offset == JOB_INT_RAWSTAT && doneAfterReads > 0) {
            if(--doneAfterReads == 0)memory[offset / 4] |= doneWord;
        }
        if(isStatus(offset)) {
            ++statusReads;
            if(activeReads > 0) {
                --activeReads;
                *value = StatusActive;
                return kIOReturnSuccess;
            }
        }
        *value = memory[offset / 4];
        return kIOReturnSuccess;
    }

    IOReturn write32(uint32_t offset, uint32_t value) override
    {
        record(offset, value);
        if(failWriteAt >= 0 && offset == static_cast<uint32_t>(failWriteAt))
            return failWriteResult;
        if(!valid(offset))return kIOReturnBadArgument;
        if(isCommand(offset)) ++commandWrites;
        if(offset == JOB_INT_CLEAR)memory[JOB_INT_RAWSTAT / 4] &= ~value;
        /* JS_STATE bit j follows the slot: START schedules the chain
         * (panfrost_job.c:270), SOFT_STOP asks it to park (:700). */
        uint32_t js = 0;
        if(jobSlotField(offset, JS_COMMAND_NEXT(0) - JS_BASE, &js)) {
            if(value == JS_COMMAND_START)
                memory[JOB_INT_JS_STATE / 4] |= 1u << js;
        } else if(jobSlotField(offset, JS_COMMAND(0) - JS_BASE, &js)) {
            if(value == JS_COMMAND_SOFT_STOP && !softStopStuck)
                memory[JOB_INT_JS_STATE / 4] &= ~(1u << js);
        }
        /* The command register clears itself once the job manager takes it. */
        memory[offset / 4] = isJobCommandNext(offset) ? 0 : value;
        return kIOReturnSuccess;
    }
};
#endif
