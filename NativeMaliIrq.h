/* SPDX-License-Identifier: GPL-2.0-only */
/* What the three interrupt status words mean: the GPU's own sources, the job
 * slot palette, and the MMU's per-address-space bits.
 *
 * Pure decoding. No register access, no interrupt claimed, nothing written: the
 * completion path still polls (docs/OPEN_QUESTIONS.md item 14), and the piece
 * that would need the XNU interrupt source is the handler around this decoder,
 * not the decoding itself. The register reads and the clear/mask writes stay
 * with NativeMaliGpuCore, NativeMaliJobSlot and NativeMaliMmu.
 *
 * Source (content-pinned, third_party/PINNED.md):
 *   panfrost_gpu.c:24-60
 *     panfrost_gpu_irq_handler(): reads GPU_FAULT_STATUS and GPU_INT_STAT,
 *     returns IRQ_NONE when the word is 0, treats GPU_IRQ_MASK_ERROR as the
 *     fatal pair, logs the fault, writes GPU_INT_MASK = 0, and finally clears
 *     exactly the bits it read (GPU_INT_CLEAR = state).
 *   panfrost_regs.h:50-68 (generated into NativeMaliRegs.h)
 *     GPU_IRQ_* and the MASK_ALL / MASK_ERROR groupings.
 *   panfrost_job.c:522-571
 *     panfrost_jm_handle_irq(): per slot, JOB_INT_MASK_DONE is BIT(js) and
 *     JOB_INT_MASK_ERR is BIT(js + 16); the same word is written to
 *     JOB_INT_CLEAR, and JS_COMMAND_NEXT = NOP cancels the staged next chain on
 *     an error.
 *   panfrost_device.h:29-32 with panfrost_job.h:48
 *     ALL_JS_INT_MASK covers GENMASK(NUM_JOB_SLOTS - 1, 0) and
 *     GENMASK(16 + NUM_JOB_SLOTS - 1, 16); NUM_JOB_SLOTS is 3.
 *   panfrost_mmu.c:868-880, 882-906
 *     panfrost_mmu_irq_handler() returns IRQ_NONE when MMU_INT_STAT is 0 and
 *     masks the line before waking the thread; the thread walks
 *     MMU_INT_RAWSTAT with as = ffs(status | (status >> 16)) - 1 and clears
 *     BIT(as) | BIT(as + 16).
 *
 * The MMU bit pair has no name in the pinned source: the handler treats the low
 * half as the fault and clears both halves together, so this decoder reports the
 * two halves separately ("fault" and "second") instead of inventing a name for
 * the high one, and keeps events on an address space the hardware does not have
 * apart from the rest.
 */
#ifndef NATIVE_MALI_IRQ_H
#define NATIVE_MALI_IRQ_H
#include "NativeMaliRegs.h"
#include <stdint.h>

class NativeMaliIrq final {
public:
    NativeMaliIrq() = delete;

    /* GPU_INT_STAT / GPU_INT_RAWSTAT. */
    struct Gpu {
        uint32_t status = 0;
        /* Set bits outside GPU_IRQ_MASK_ALL, which the source never masks and
         * would never clear either (it writes the whole word back). */
        uint32_t unknown = 0;

        bool fault() const { return (status & GPU_IRQ_FAULT) != 0; }
        bool multipleFault() const
        {
            return (status & GPU_IRQ_MULTIPLE_FAULT) != 0;
        }
        bool resetCompleted() const
        {
            return (status & GPU_IRQ_RESET_COMPLETED) != 0;
        }
        bool powerChanged() const
        {
            return (status & GPU_IRQ_POWER_CHANGED) != 0;
        }
        bool powerChangedAll() const
        {
            return (status & GPU_IRQ_POWER_CHANGED_ALL) != 0;
        }
        bool perfcntSample() const
        {
            return (status & GPU_IRQ_PERFCNT_SAMPLE_COMPLETED) != 0;
        }
        bool cleanCaches() const
        {
            return (status & GPU_IRQ_CLEAN_CACHES_COMPLETED) != 0;
        }
        /* GPU_IRQ_MASK_ERROR (panfrost_gpu.c:37): the handler reads the fault
         * status and the fault address, logs them and masks the line. */
        bool error() const { return (status & GPU_IRQ_MASK_ERROR) != 0; }
        /* At least one bit the driver knows about. A word with only `unknown`
         * bits set still makes the source clear them. */
        bool any() const { return status != 0; }
    };

    /* JOB_INT_STAT / JOB_INT_RAWSTAT, with the per-slot bits folded onto one
     * bit per slot: `done` and `error` both use BIT(js), so a handler can test
     * both with the slot number instead of two offsets. */
    struct Jobs {
        uint32_t status = 0;
        uint32_t done = 0;
        uint32_t error = 0;
        /* Slots outside the driver's slot count (reserved bits), or bits the
         * two halves do not cover: `slots` is not a power-of-two mask. */
        uint32_t unknown = 0;

        bool any() const { return (done | error) != 0; }
        bool doneOn(uint32_t js) const
        {
            return js < 32 && ((done >> js) & 1u) != 0;
        }
        bool errorOn(uint32_t js) const
        {
            return js < 32 && ((error >> js) & 1u) != 0;
        }
    };

    /* MMU_INT_STAT / MMU_INT_RAWSTAT. All three masks are indexed by address
     * space number; `absent` holds events for an AS outside `present`, which
     * means the status word and GPU_AS_PRESENT disagree and is worth reporting
     * rather than walking. */
    struct Mmu {
        uint32_t status = 0;
        uint32_t fault = 0;    /* BIT(as): the low half */
        uint32_t second = 0;   /* BIT(as): the BIT(as + 16) half */
        uint32_t absent = 0;

        bool any() const { return status != 0; }
        bool faultOn(uint32_t as) const
        {
            return as < 32 && ((fault >> as) & 1u) != 0;
        }
        bool secondOn(uint32_t as) const
        {
            return as < 32 && ((second >> as) & 1u) != 0;
        }
    };

    /* The source's zero test (panfrost_gpu.c:34-35). */
    static Gpu decodeGpu(uint32_t status);
    /* `slots` is the number of job slots the device has (NUM_JOB_SLOTS, 3).
     * Anything above it is `unknown`; 0 or more than 16 is not a slot count a
     * 32-bit palette can hold, and the caller gets every bit as unknown. */
    static Jobs decodeJobs(uint32_t status, uint32_t slots);
    /* `present` is GPU_AS_PRESENT (a 16-bit mask). */
    static Mmu decodeMmu(uint32_t status, uint32_t present);
    /* The word the MMU handler writes to MMU_INT_CLEAR for one AS,
     * panfrost_mmu.c:890. */
    static uint32_t mmuClearMask(uint32_t as);
};

#endif
