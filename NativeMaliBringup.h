/* SPDX-License-Identifier: GPL-2.0-only */
/* One place that puts M1-M4 in order: read the platform, power the GPU, and
 * bring the core and its MMU out of reset.
 *
 * Every step already exists as its own tested object (NativeMaliPower,
 * NativeMaliGpuCore, NativeMaliMmu). This file adds no register knowledge at
 * all: it only fixes the order and the failure handling, so the IOKit glue has
 * one call to make instead of six, and so the order is testable on the host
 * without a GPU.
 *
 * Source for the order (content-pinned, third_party/PINNED.md):
 *   drivers/gpu/drm/panfrost/panfrost_device.c:260-332
 *     panfrost_device_init(): pm domain, reset, clocks, devfreq, regulators and
 *     the ioremap (:276-310), then panfrost_gpu_init() (:312), then
 *     panfrost_mmu_init() (:316), then panfrost_jm_init() (:320). The GPU is
 *     identified *before* the MMU exists, which is why probe() has to come
 *     before any address space is allocated here.
 *   panfrost_device.c:510-524
 *     panfrost_device_reset(): panfrost_gpu_soft_reset(), then
 *     panfrost_gpu_power_on(), then panfrost_mmu_reset(), then
 *     panfrost_jm_reset_interrupts(). That is exactly start()'s order after the
 *     M2 block is up.
 *   panfrost_mmu.c:345-365
 *     panfrost_mmu_reset() clears as_alloc_mask/as_faulty_mask and forgets each
 *     mmu->as, i.e. NativeMaliMmu::resetAllocation().
 *   panfrost_device.c:549-568
 *     panfrost_device_runtime_suspend(): refuse while a job is in flight
 *     (panfrost_jm_is_idle), then panfrost_gpu_power_off(), then the clocks.
 *   panfrost_device.c:388-408
 *     panfrost_device_fini() tears down in the reverse of init: gem, perfcnt,
 *     jm, mmu, gpu, devfreq, regulator, clk, reset, pm domain. stop() follows
 *     the same reverse shape for the part it owns: GPU blocks, then the MFG
 *     domains. The address spaces are gone before stop() is called.
 *   mtk_gpufreq_core.c:415-496 (the MT6768 vendor file)
 *     the clock gate goes away before the MTCMOS domains, which
 *     NativeMaliPower::tearDown() already encodes.
 *
 * Read before write, literally: start() runs the read-only M1 probe first and
 * refuses to write anything if it fails. That is roadmap rule "read SPM before
 * the GPU" as a precondition, not a comment.
 *
 * Address spaces are *not* allocated here. panfrost takes one per submission
 * (panfrost_job_hw_submit(), panfrost_job.c:222) and puts it back when the job
 * is reaped (panfrost_job.c:494, :515); a client's context is torn down by
 * disabling the AS and clearing its allocation bit
 * (panfrost_mmu_release_ctx(), panfrost_mmu.c:722-743). This driver keeps the
 * AS for the whole time a client is open instead of refcounting it per job,
 * which is the same lifetime with one programme step instead of two. Either
 * way the AS belongs to the owner of the page tables, not to the power/reset
 * sequence, so start() stops after panfrost_mmu_reset() and stop() refuses to
 * power the block down while any AS is still allocated.
 *
 * Fail closed: every failure quiesces in reverse (GPU blocks, MFG domains)
 * and returns the *original* error. The teardown's own error is
 * reported through an out parameter rather than replacing the cause, so nothing
 * is swallowed and the caller can log both.
 *
 * Writes stay behind the per-milestone switches: start() needs
 * NATIVE_MALI_M2_WRITES and NATIVE_MALI_M3_WRITES; NativeMaliMmu::enable(),
 * which the AS owner calls, needs NATIVE_MALI_M4_WRITES. With the defaults all
 * of them are kIOReturnUnsupported and nothing is written.
 *
 * Not here on purpose: the job slots and the VM. A slot belongs to whichever
 * client submits, and a VM's page tables come from a caller-owned
 * NativeMaliPageSource; bolting either onto bring-up would mean guessing the
 * M7 shape. stop() therefore asks the caller whether jobs are outstanding
 * instead of asking a job object it does not own.
 */
#ifndef NATIVE_MALI_BRINGUP_H
#define NATIVE_MALI_BRINGUP_H
#include "NativeMaliGpuCore.h"
#include "NativeMaliMmu.h"
#include "NativeMaliPower.h"
#include "NativeMaliVm.h"
#include <IOKit/IOTypes.h>
#include <stdint.h>

struct NativeMaliBringupBlocks {
    NativeMaliPowerBlocks power;
    /* The 0x13040000 window: GPU core, job slots and MMU live in it. */
    NativeMaliRegisterTarget *gpu = nullptr;
};

/* What the two read-only probes saw. Filled for as far as the sequence got, so
 * a failure still leaves the numbers the caller needs to log. */
struct NativeMaliBringupState {
    NativeMaliPowerObservation power;
    NativeMaliGpuObservation gpu;
};

class NativeMaliBringup final {
public:
    /* panfrost_jm_is_idle() has no counterpart here because this object does
     * not own the job slots; the caller states it, and stop() refuses with
     * kIOReturnBusy without writing anything when a job is still in flight. */
    enum class Jobs { Idle, Outstanding };

    NativeMaliBringup() = default;
    ~NativeMaliBringup() = default;
    NativeMaliBringup(const NativeMaliBringup &) = delete;
    NativeMaliBringup &operator=(const NativeMaliBringup &) = delete;

    /* Requires all five power windows and the GPU window; re-preparing with the
     * same blocks is allowed, a different set is refused (the components do
     * that themselves, this only forwards it). */
    IOReturn prepare(const NativeMaliBringupBlocks &blocks);
    bool prepared() const;

    bool started() const { return powered_ && gpuOn_; }

    /* The read-only probes, in the order they ran. Valid as far as start() got;
     * observation() is meaningless before the GPU probe returned success. */
    NativeMaliPower &power() { return power_; }
    NativeMaliGpuCore &gpuCore() { return gpu_; }
    NativeMaliMmu &mmu() { return mmu_; }
    const NativeMaliGpuObservation &observation() const { return observation_; }

    /* Read the platform (M1), then voltage -> clock -> domains -> gate (M2),
     * then probe -> soft reset -> power on (M3), then MMU present and reset
     * (M4).
     *
     *   kIOReturnSuccess      `state` is filled and the MMU knows its slots.
     *   kIOReturnUnsupported  writes compiled out (the default), or the GPU id
     *                         is not in the pinned table; `state->gpu` still
     *                         holds what the block reported.
     *   kIOReturnBusy         already started, or a previous stop() failed and
     *                         left the block half down.
     *   other                 the failing step's error, with everything
     *                         already brought up quiesced.
     *
     * No address space is allocated: the AS owner calls
     * NativeMaliMmu::allocate()/enable()/disable()/release() on mmu(). M4's
     * work here is panfrost_mmu_reset() (clear the allocation bitmap the last
     * boot may have left and record GPU_AS_PRESENT), which is exactly what
     * panfrost_device_reset() does after power-on (panfrost_device.c:517).
     * `state` may be null when the caller only needs success/failure;
     * `teardownError` receives the rollback's own error (kIOReturnSuccess when
     * there was nothing to roll back or it worked). */
    IOReturn start(NativeMaliBringupState *state,
        IOReturn *teardownError = nullptr);

    /* Undo start() in the order panfrost_device_fini() uses: GPU blocks, then
     * the MFG domains. The address spaces are already gone: the owner closed
     * them before asking for this.
     *
     * kIOReturnBusy (nothing written) when `jobs` is Outstanding, or when the
     * MMU still has an allocated slot - powering the block down under an AS
     * the GPU may still be walking is what the AS owner must prevent, and
     * failing here is better than guessing that it is safe.
     * Every step is attempted even after a failure and the first error is
     * returned; a step that failed stays marked as still up, so this can be
     * retried and started() keeps telling the truth in the meantime.
     * kIOReturnSuccess when nothing was started (idempotent).
     *
     * The one refusal that is not a step failure: an AS that is still
     * allocated means its owner has not finished tearing it down, so stop()
     * writes nothing at all and answers kIOReturnBusy. That keeps the number
     * from being handed to another client while the GPU may still be walking
     * those tables, and it keeps this object from having to remember an AS it
     * does not own. */
    IOReturn stop(Jobs jobs, IOReturn *teardownError = nullptr);

private:
    /* Reverse-order teardown shared by stop() and start()'s failure path.
     * Returns the first error; never skips a step because an earlier one
     * failed. */
    IOReturn quiesce(IOReturn *teardownError);

    NativeMaliPower power_;
    NativeMaliGpuCore gpu_;
    NativeMaliMmu mmu_;
    NativeMaliGpuObservation observation_{};
    /* One flag per step that has to be undone, in the order it was done. */
    bool powered_ = false;
    bool gpuOn_ = false;
};
#endif
