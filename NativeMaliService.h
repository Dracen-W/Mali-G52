/* SPDX-License-Identifier: GPL-2.0-only */
/* The device as one object: bring-up, the shared job slots, one address space
 * per client, and the reset that puts all of them back.
 *
 * Each of those pieces already exists and is host-tested on its own
 * (NativeMaliBringup, NativeMaliJobSlot, NativeMaliUserClient, NativeMaliVm,
 * NativeMaliMmu). This file adds no register knowledge either. It fixes the
 * two things that only a device-wide owner can decide:
 *
 *   - an address space belongs to a client for as long as the client is open.
 *     panfrost takes one per submission (panfrost_job_hw_submit(),
 *     panfrost_job.c:222) and puts it back when the job is reaped
 *     (panfrost_job.c:494, :515), and closing the context disables it and
 *     clears its allocation bit (panfrost_mmu_release_ctx(),
 *     panfrost_mmu.c:722-743). This driver opens the AS once with the client
 *     and closes it at the same time as the client, which is one programme
 *     step instead of two per job. The number is kept across a reset: the
 *     hardware forgot the TRANSTAB, not who owns the slot.
 *   - a GPU reset has to be followed by re-programming every open client's
 *     address space, because panfrost_device_reset() cleared the hardware
 *     (panfrost_device.c:510-524).
 *
 * Reset order, from panfrost_reset() (panfrost_job.c:658-760):
 *   drm_sched_stop() on every queue (:684), JOB_INT_MASK = 0 (:694), the
 *   per-slot JS_COMMAND_NEXT = NOP / JS_COMMAND = SOFT_STOP pair (:699-700),
 *   a JS_STATE poll (:704-706), then panfrost_device_reset() (soft reset,
 *   power on, panfrost_mmu_reset(), panfrost_jm_reset_interrupts(), :735).
 *
 * What this driver does instead of each of those, and why:
 *   - There is no scheduler and no job object, so "stop the queues" and
 *     "signal the in-flight fences with an error" are one pure state drop:
 *     NativeMaliJobSlot::abandonOutstanding(). Clients learn about it through
 *     NativeMaliUserClient::noteReset(), which turns exactly the fence that was
 *     in flight into an error fence.
 *   - JOB_INT_MASK stays untouched: this driver never unmasks it (completion is
 *     polled, docs/TODO.md M6), so there is nothing to mask.
 *   - The per-slot NOP/SOFT_STOP + JS_STATE quiesce is
 *     NativeMaliJobSlot::quiesce(), run before softReset(). A soft-stop timeout
 *     is not a reset failure: the source logs it and resets anyway
 *     (panfrost_job.c:708-709), so it is counted in `softStopTimeouts_` and
 *     the reset continues. The ABI has no field for the count; the glue logs it.
 *   - Resubmission does not exist either: panfrost re-queues jobs that never
 *     started (:748-749); here the caller sees an error fence and decides. A
 *     silently restarted job would break the fence the ABI handed out.
 *
 * Fail closed: every failure returns the offending step's own error, leaves
 * the object in a state where `clientUsable()` says which clients may still be
 * driven, and never reports the device ready when it is not. A reset that
 * fails anywhere before the re-enable completes marks the whole service
 * unusable until stop()/start(); closeClient() still works so the glue can
 * tear the clients down.
 *
 * Not here: the IOUserClient itself, the Info.plist, DT parsing, MMIO mapping
 * and the memory/cache backend (docs/OPEN_QUESTIONS.md item 5).
 */
#ifndef NATIVE_MALI_SERVICE_H
#define NATIVE_MALI_SERVICE_H
#include "NativeMaliBringup.h"
#include "NativeMaliJobSlot.h"
#include "NativeMaliUserClient.h"
#include <stddef.h>
#include <stdint.h>

/* One entry per open client. The objects themselves belong to the IOKit glue:
 * a NativeMaliUserClient carries its own 64-entry BO table, so it is allocated
 * per connection rather than pooled here. This limit is the number of address
 * spaces the MMU can name, because a client without one cannot exist in this
 * design (NativeMaliMmuMaxAs, the width of GPU_AS_PRESENT). */
enum : size_t { NativeMaliServiceMaxClients = NativeMaliMmuMaxAs };

class NativeMaliService final : private NativeMaliResetTarget {
    struct Client {
        NativeMaliUserClient *client = nullptr;
        NativeMaliVm *vm = nullptr;
        uint32_t as = 0;
        /* False once a reset could not re-enable this client's AS; the glue
         * must close it instead of dispatching more requests. */
        bool usable = false;
    };

    NativeMaliBringup bringup_;
    NativeMaliJobSlot jobs_;
    NativeMaliGpuObservation observation_{};
    Client clients_[NativeMaliServiceMaxClients]{};
    size_t clientCount_ = 0;
    /* Set when a reset failed; cleared by the next successful start(). */
    bool resetFailed_ = false;
    /* Diagnostic only: resets that continued past a soft-stop timeout. */
    uint64_t softStopTimeouts_ = 0;

    size_t indexOf(const NativeMaliUserClient *client) const;
    bool anyOutstanding() const;
    IOReturn rollbackOpen(uint32_t as, NativeMaliVm *vm, IOReturn first,
        IOReturn *teardownError);
    void forget(size_t index);
    /* NativeMaliResetTarget: the ABI Reset selector, forwarded by each client
     * the service opened. One spelling of reset(), not a second sequence. */
    IOReturn resetDevice() override { return reset(); }
public:
    NativeMaliService() = default;
    ~NativeMaliService() = default;
    NativeMaliService(const NativeMaliService &) = delete;
    NativeMaliService &operator=(const NativeMaliService &) = delete;

    /* Requires the five power windows and the GPU window, as bring-up does.
     * The job slots share the GPU window and the MMU, so they are prepared
     * here and nowhere else. */
    IOReturn prepare(const NativeMaliBringupBlocks &blocks);
    bool prepared() const;

    /* M1-M4 as NativeMaliBringup::start() runs them, plus the two probe facts
     * the job slots need (GPU_JS_PRESENT and GPU_SHADER_PRESENT). No address
     * space is allocated: clients own those. Refuses with kIOReturnBusy while
     * a client is still open, and refuses to start on top of a failed reset. */
    IOReturn start(NativeMaliBringupState *state,
        IOReturn *teardownError = nullptr);
    bool started() const { return bringup_.started(); }
    /* Refuses (kIOReturnBusy, nothing written) while a client is open or a job
     * slot is still outstanding, and otherwise tears the GPU down in the
     * reverse order of start().
     *
     * A job cannot outlive its client through the service: the slot refuses a
     * submission whose AS is not allocated, and closeClient() waits for the
     * client's own jobs. The job check is still here because only a bug in the
     * AS owner - releasing a number while its chain is still queued - could
     * reach it, and powering the block down under that chain is worse than
     * answering Busy. */
    IOReturn stop(IOReturn *teardownError = nullptr);

    /* The two objects the glue and the clients are built on. The service is
     * their only owner: prepare(), start() and stop() are the only ways in. */
    NativeMaliBringup &bringup() { return bringup_; }
    NativeMaliJobSlot &jobs() { return jobs_; }
    const NativeMaliGpuObservation &observation() const { return observation_; }
    size_t clients() const { return clientCount_; }
    /* Resets that went ahead after NativeMaliJobSlot::quiesce() hit its 10 ms
     * soft-stop timeout. panfrost only logs "Soft-stop failed" and resets
     * anyway (panfrost_job.c:708-709); the ABI has no field for it, so the
     * glue reads this for logging. Cleared by a successful start(). */
    uint64_t softStopTimeouts() const { return softStopTimeouts_; }

    /* Open one client: allocate the next free AS, prepare its page tables from
     * `tables`, program TRANSTAB (NativeMaliMmu::enable()), then open `client`
     * on that AS. The caller keeps `client` and `vm` alive until
     * closeClient() returns; this object stores pointers only.
     *
     * Refusals before any write: not started, a null client/vm/tables/buffers,
     * the same client twice (Busy), the table full (NoResources), or a failed
     * reset (NotReady).
     *
     * `buffers` and `cache` are the client's BO memory source and the maintainer
     * for a non-coherent buffer; `coherent` is the GPU fact, so it applies to
     * the page-table walker and to the buffers alike.
     *
     * A failure after the AS was taken rolls back in reverse: disable, release
     * the tables, give the slot back. The original error is returned; the
     * rollback's own error goes to `teardownError`. A disable that fails leaves
     * the AS allocated on purpose - handing a slot the GPU may still be walking
     * to another client is worse than leaking it until stop() notices. */
    IOReturn openClient(NativeMaliUserClient *client, NativeMaliVm *vm,
        NativeMaliPageSource *tables, NativeMaliMemorySource *buffers,
        NativeMaliCacheMaintainer *cache, bool coherent, uint64_t maxBoBytes,
        IOReturn *teardownError = nullptr);

    /* Release the client's objects first (it is closing, so releaseAll() drops
     * the CPU tokens and frees every BO in slot order), then disable the AS and
     * give the slot back, then release the page tables. Any failure stops the
     * sequence with the client still registered and the AS still allocated, so
     * the call can be repeated - and stop() keeps refusing in the meantime.
     * kIOReturnBadArgument for a client this service did not open. */
    IOReturn closeClient(NativeMaliUserClient *client);

    /* The reset sequence described at the top of this file. Requires start().
     *
     * With no client open this is bring-up's own reset: quiesce the job
     * manager (NOP/SOFT_STOP + JS_STATE poll), soft reset, power on, forget the
     * allocation bitmap, clear the job event palette, drop the in-flight slot
     * state. A soft-stop timeout is counted in `softStopTimeouts()` and does
     * not fail the reset, exactly as the source logs it and resets anyway.
     *
     * With clients open it additionally re-claims each client's AS number and
     * programs it again with the same page tables, and tells every client that
     * its in-flight fences are dead (noteReset()). A failure anywhere returns
     * that step's error and makes `clientUsable()` false for every client, so
     * nothing is submitted on a GPU whose state this driver no longer trusts.
     * From then on reset() itself is refused (kIOReturnNotReady) until
     * close/stop/start: a second reset would report success while every client
     * is still unusable.
     * The hardware reset itself cannot be undone.
     *
     * This is also the wire ABI's Reset selector: openClient() gives each
     * client this object as its NativeMaliResetTarget, so NativeMaliUserClient::
     * dispatch() forwards that method here. Any connected client may call it;
     * see NativeMaliABI.h for why the method is device-wide. */
    IOReturn reset();

    /* True only while this client may be driven: it is registered here, its AS
     * was re-enabled successfully by the last reset, and no reset failed since.
     * The IOKit glue checks this before dispatching an ABI method; false means
     * kIOReturnNotReady and no hardware access. */
    bool clientUsable(const NativeMaliUserClient *client) const;
};
#endif
