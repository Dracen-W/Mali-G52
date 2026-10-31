/* SPDX-License-Identifier: GPL-2.0-only */
/* The device-wide bookkeeping; every register fact and its citation is in the
 * files this one calls. The citations for the order are in NativeMaliService.h.
 */
#include "NativeMaliService.h"

size_t NativeMaliService::indexOf(const NativeMaliUserClient *client) const
{
    for(size_t index = 0; index < clientCount_; ++index)
        if(clients_[index].client == client)return index;
    /* clientCount_ is one past the last entry, so it is never a valid index. */
    return clientCount_;
}

bool NativeMaliService::anyOutstanding() const
{
    for(uint32_t js = 0; js < NativeMaliJobSlotCount; ++js)
        if(jobs_.outstanding(js))return true;
    return false;
}

IOReturn NativeMaliService::prepare(const NativeMaliBringupBlocks &blocks)
{
    if(!blocks.gpu)return kIOReturnBadArgument;
    const IOReturn result = bringup_.prepare(blocks);
    if(result != kIOReturnSuccess)return result;
    /* The slots live in the same window and check the same MMU, so they are
     * prepared with the objects bring-up just prepared, not with copies. */
    return jobs_.prepare(blocks.gpu, &bringup_.power(), &bringup_.mmu());
}

bool NativeMaliService::prepared() const
{
    return bringup_.prepared() && jobs_.prepared();
}

IOReturn NativeMaliService::start(NativeMaliBringupState *state,
    IOReturn *teardownError)
{
    if(teardownError)*teardownError = kIOReturnSuccess;
    if(!prepared())return kIOReturnNotReady;
    if(clientCount_ != 0)return kIOReturnBusy;

    NativeMaliBringupState local;
    IOReturn result = bringup_.start(&local, teardownError);
    if(result != kIOReturnSuccess)return result;

    /* GPU_JS_PRESENT and GPU_SHADER_PRESENT come from the probe that start()
     * just ran, exactly where panfrost_jm_init() takes them
     * (panfrost_device.c:320). A GPU with no job slot cannot run anything this
     * driver offers, so a refusal here tears the block down again. */
    result = jobs_.setPresent(local.gpu.jsPresent, local.gpu.shaderPresent);
    if(result != kIOReturnSuccess) {
        bringup_.stop(NativeMaliBringup::Jobs::Idle, teardownError);
        return result;
    }

    observation_ = local.gpu;
    resetFailed_ = false;
    softStopTimeouts_ = 0;
    if(state)*state = local;
    return kIOReturnSuccess;
}

IOReturn NativeMaliService::stop(IOReturn *teardownError)
{
    if(teardownError)*teardownError = kIOReturnSuccess;
    /* The AS owners close first. A client still registered means an address
     * space may still be programmed, which is bring-up's own refusal; saying it
     * here keeps the reason visible instead of surfacing as "MMU busy". */
    if(clientCount_ != 0)return kIOReturnBusy;
    if(anyOutstanding())return kIOReturnBusy;

    const IOReturn result = bringup_.stop(NativeMaliBringup::Jobs::Idle,
        teardownError);
    if(result == kIOReturnSuccess) {
        observation_ = NativeMaliGpuObservation{};
        resetFailed_ = false;
    }
    return result;
}

IOReturn NativeMaliService::rollbackOpen(uint32_t as, NativeMaliVm *vm,
    IOReturn first, IOReturn *teardownError)
{
    if(teardownError)*teardownError = kIOReturnSuccess;
    /* panfrost_mmu_release_ctx() order (panfrost_mmu.c:722-743): disable, give
     * the number back, then free the tables. A failed disable leaves both the
     * slot and the tables alone on purpose. */
    IOReturn result = bringup_.mmu().disable(as);
    if(result != kIOReturnSuccess) {
        if(teardownError)*teardownError = result;
        return first;
    }
    result = bringup_.mmu().release(as);
    if(result != kIOReturnSuccess) {
        if(teardownError)*teardownError = result;
        return first;
    }
    vm->reset();
    return first;
}

IOReturn NativeMaliService::openClient(NativeMaliUserClient *client,
    NativeMaliVm *vm, NativeMaliPageSource *tables,
    NativeMaliMemorySource *buffers, NativeMaliCacheMaintainer *cache,
    bool coherent, uint64_t maxBoBytes, IOReturn *teardownError)
{
    if(teardownError)*teardownError = kIOReturnSuccess;
    if(!client || !vm || !tables || !buffers)return kIOReturnBadArgument;
    if(!started())return kIOReturnNotReady;
    if(resetFailed_)return kIOReturnNotReady;
    if(indexOf(client) != clientCount_)return kIOReturnBusy;
    if(clientCount_ == NativeMaliServiceMaxClients)return kIOReturnNoResources;
    if(client->open())return kIOReturnBusy;

    uint32_t as = 0;
    IOReturn result = bringup_.mmu().allocate(&as);
    if(result != kIOReturnSuccess)return result;

    /* The page tables are built from the geometry the probe reported.
     * panfrost_mmu_ctx_create() does the same with the same two words
     * (panfrost_mmu.c:207-217). */
    result = vm->prepare(tables, observation_.ias, observation_.oas, coherent,
        cache);
    if(result != kIOReturnSuccess) {
        bringup_.mmu().release(as);
        return result;
    }

    result = bringup_.mmu().enable(as, *vm);
    if(result != kIOReturnSuccess) {
        /* enable() has already tried to disable a partially written AS. */
        vm->reset();
        bringup_.mmu().release(as);
        return result;
    }

    result = client->open(buffers, cache, coherent, vm, &bringup_.mmu(), as,
        &jobs_, &bringup_.gpuCore(), observation_, maxBoBytes, this);
    if(result != kIOReturnSuccess)
        return rollbackOpen(as, vm, result, teardownError);

    clients_[clientCount_++] = Client{client, vm, as, true};
    return kIOReturnSuccess;
}

void NativeMaliService::forget(size_t index)
{
    --clientCount_;
    clients_[index] = clients_[clientCount_];
    clients_[clientCount_] = Client{};
}

IOReturn NativeMaliService::closeClient(NativeMaliUserClient *client)
{
    const size_t index = indexOf(client);
    if(index == clientCount_)return kIOReturnBadArgument;

    Client &entry = clients_[index];
    /* panfrost_mmu_release_ctx() guards its disable with pm_runtime_active()
     * (panfrost_mmu.c:731-732): a block that is not powered has nothing to
     * program and cannot still be walking the tables. Without that guard, a
     * reset whose power-on did not bring the block back leaves releaseAll()'s
     * flush (gpuReadable()) and disable() failing forever, so the client could
     * never close. The buffers go back through the no-register path, the
     * number and the tables are still released, and no register is touched.
     *
     * Only kIOReturnNotReady means "the block is not there" (a domain, the
     * clock mux or the gate is missing). A failed register read is a different
     * answer: the state is unknown, so keep the client and return it. */
    if(bringup_.power().gpuReadable() == kIOReturnNotReady) {
        if(entry.client->open()) {
            const IOReturn result = entry.client->abandon();
            if(result != kIOReturnSuccess)return result;
        }
        IOReturn result = bringup_.mmu().release(entry.as);
        if(result != kIOReturnSuccess)return result;
        entry.vm->reset();
        forget(index);
        return kIOReturnSuccess;
    }

    /* The connection is going away, so this is releaseAll(): CPU tokens first,
     * then every BO. If it fails the client stays exactly as it was and the
     * call can be repeated. */
    if(entry.client->open()) {
        const IOReturn result = entry.client->releaseAll();
        if(result != kIOReturnSuccess)return result;
    }

    IOReturn result = bringup_.mmu().disable(entry.as);
    if(result != kIOReturnSuccess)return result;
    result = bringup_.mmu().release(entry.as);
    if(result != kIOReturnSuccess)return result;
    entry.vm->reset();
    forget(index);
    return kIOReturnSuccess;
}

IOReturn NativeMaliService::reset()
{
    if(!started())return kIOReturnNotReady;
    /* A reset that failed left the GPU in a state this driver does not trust.
     * Resetting again would destroy whatever survived without knowing what it
     * is; the documented way out is close, stop, start. Refusing here keeps a
     * second reset from reporting success while every client is unusable. */
    if(resetFailed_)return kIOReturnNotReady;

    /* panfrost_reset() stops its schedulers and quiesces the job manager
     * before it touches the reset lines (panfrost_job.c:684, :697-709). There
     * is no scheduler here, so the queue stop has no counterpart; the job
     * manager half is NativeMaliJobSlot::quiesce(). */
    const IOReturn quiet = jobs_.quiesce();
    if(quiet == kIOReturnTimeout) {
        /* The source logs "Soft-stop failed" and resets anyway (:708-709).
         * Keep that decision, but count it so it is not lost: the ABI has no
         * field for it, the glue can log the counter. */
        ++softStopTimeouts_;
    } else if(quiet != kIOReturnSuccess) {
        resetFailed_ = true;
        return quiet;
    }

    IOReturn result = bringup_.gpuCore().softReset();
    if(result != kIOReturnSuccess) {
        resetFailed_ = true;
        return result;
    }

    /* panfrost_device_reset() from here on (panfrost_device.c:510-524). The
     * soft reset destroyed every in-flight job and every programmed AS, whether
     * or not the power-on that follows succeeds. */
    const IOReturn power = bringup_.gpuCore().powerOn();
    bringup_.mmu().resetAllocation();
    for(size_t index = 0; index < clientCount_; ++index)
        clients_[index].usable = false;

    /* The source's order is mmu_reset() and then jm_reset_interrupts(); the
     * event palette is the half that needs a readable GPU. */
    const IOReturn clear = power == kIOReturnSuccess
        ? jobs_.resetInterrupts() : power;
    jobs_.abandonOutstanding();
    for(size_t index = 0; index < clientCount_; ++index)
        clients_[index].client->noteReset();

    /* Put every number back regardless of how the reset went. The slot still
     * belongs to that client - nothing else may be given it - and closeClient()
     * needs the allocation to unmap the client's BOs and tear the AS down, even
     * when the reset could not finish. Not enabling them yet keeps the source's
     * order for the success path. */
    IOReturn first = kIOReturnSuccess;
    for(size_t index = 0; index < clientCount_; ++index) {
        const IOReturn reclaimed = bringup_.mmu().reclaim(clients_[index].as);
        if(reclaimed != kIOReturnSuccess && first == kIOReturnSuccess)
            first = reclaimed;
    }
    if(first != kIOReturnSuccess) {
        resetFailed_ = true;
        return first;
    }
    if(power != kIOReturnSuccess) {
        resetFailed_ = true;
        return power;
    }
    if(clear != kIOReturnSuccess) {
        resetFailed_ = true;
        return clear;
    }

    /* panfrost resubmits the jobs that never started (panfrost_job.c:748-749)
     * and re-enables job interrupts (:754, :757). Here the jobs are the
     * caller's: its fences are dead, and what has to be restored is each open
     * client's address space. */
    for(size_t index = 0; index < clientCount_; ++index) {
        Client &entry = clients_[index];
        const IOReturn restored = bringup_.mmu().enable(entry.as, *entry.vm);
        if(restored == kIOReturnSuccess) {
            entry.usable = true;
        } else {
            entry.usable = false;
            if(first == kIOReturnSuccess)first = restored;
        }
    }
    if(first != kIOReturnSuccess) {
        /* The GPU is powered and reset, but not every client can run. Nothing
         * is reported usable; the glue closes the clients and start()s again. */
        resetFailed_ = true;
        return first;
    }
    return kIOReturnSuccess;
}

bool NativeMaliService::clientUsable(const NativeMaliUserClient *client) const
{
    const size_t index = indexOf(client);
    if(index == clientCount_)return false;
    return started() && !resetFailed_ && clients_[index].usable;
}
