/* SPDX-License-Identifier: GPL-2.0-only */
/* The XNU surface: one IOService for the GPU node, one IOUserClient per
 * connection, and the dispatch that turns the wire ABI into the pure
 * NativeMaliService / NativeMaliUserClient calls.
 *
 * UNVERIFIED(XNU) - this file has never been compiled against XNU headers
 * (only -fsyntax-only against local IOKit stubs, which prove nothing about the
 * real signatures). There is no kext build in this repository, and tests/run.sh
 * does not see xnu/. Every IOKit call
 * below is used with the same shape in NativeAdreno's in-tree driver (MIT, see
 * NOTICE) and every Mali-specific assumption is tagged and listed in
 * docs/UNVERIFIED.md. Do not treat a successful host test run as evidence that
 * this file works.
 *
 * Division of labour, the same one docs/TODO.md describes:
 *   - the pure layers decide everything about registers, page tables, slots,
 *     fences and ABI records;
 *   - this file maps windows, allocates memory, copies records and holds the
 *     one lock that serialises every method call.
 *
 * Order in start(): map, prepare, start, publish - and refuse to publish when
 * start() did not reach the ready state. With the default write switches
 * (NATIVE_MALI_M2_WRITES/NATIVE_MALI_M3_WRITES == 0) start() returns
 * kIOReturnUnsupported after the read-only probe; the probe output is logged and
 * the service is not published. That is the first thing to run on the phone
 * (docs/INTEGRATION.md).
 */
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <kern/task.h>
#include <string.h>

#include "NativeMaliABI.h"
#include "NativeMaliBoard.h"
#include "NativeMaliGpuCore.h"
#include "NativeMaliPower.h"
#include "NativeMaliService.h"
#include "NativeMaliUserClient.h"
#include "NativeMaliXnuSupport.h"

namespace {

/* The Mali node and the five blocks M2 programs. The GPU window is the
 * provider's own reg; the others are separate DT nodes whose bases are fixed in
 * the vendor DTS and quoted in NativeMaliPower.h/NativeMaliPwrap.h.
 *
 * UNVERIFIED(XNU): mapping them by physical address assumes nothing else owns
 * those ranges on this kernel, and that IODeviceMemory::withRange is the right
 * way to get them; a DT phandle walk is the alternative. The bases themselves
 * are sourced (NativeMaliPower.h section 1), the access is not. */
constexpr uint64_t kGpuBase = 0x13040000;      /* NativeMaliBoard.h */
constexpr uint64_t kGpuBytes = 0x4000;
constexpr uint64_t kSpmBase = 0x10006000;      /* NativeMaliPower.h section 1 */
constexpr uint64_t kInfracfgBase = 0x10001000;
constexpr uint64_t kTopckgenBase = 0x10000000;
constexpr uint64_t kMfgcfgBase = 0x13000000;
constexpr uint64_t kPwrapBase = 0x1000D000;
constexpr uint64_t kPowerWindowBytes = 0x1000;

bool advertisesMali(IOService *provider)
{
    OSObject *property = provider ? provider->getProperty("compatible") : nullptr;
    OSData *data = OSDynamicCast(OSData, property);
    if(!data || !data->getBytesNoCopy() || !data->getLength())return false;
    const uint8_t *text = static_cast<const uint8_t *>(data->getBytesNoCopy());
    const size_t length = data->getLength();
    static const char needle[] = "mediatek,mali";
    for(size_t at = 0; at + sizeof(needle) - 1 <= length; ++at)
        if(memcmp(text + at, needle, sizeof(needle) - 1) == 0)return true;
    return false;
}

uint32_t cellProperty(IOService *provider, const char *name, uint32_t fallback)
{
    OSData *data = OSDynamicCast(OSData, provider ? provider->getProperty(name) : nullptr);
    if(!data || data->getLength() < 4 || !data->getBytesNoCopy())return fallback;
    return *static_cast<const uint32_t *>(data->getBytesNoCopy());
}

/* One fixed power/clock window. The IOMemoryMap stays in the service; the
 * register target only borrows the virtual address. The map is a raw pointer
 * with an explicit release in free()/stop(): an IOKit object's C++ destructor
 * (and therefore a member's destructor, such as OSSharedPtr's) is not
 * guaranteed to run on this kernel, so every owned reference is dropped by
 * hand, the same way NativeAdrenoService.cpp does it. */
IOReturn mapFixedWindow(uint64_t base, uint64_t bytes,
    IOMemoryMap **map, volatile uint8_t **virtual_address)
{
    IODeviceMemory *memory = IODeviceMemory::withRange(base, bytes);
    if(!memory)return kIOReturnNoMemory;
    *map = memory->map(kIOMapAnywhere | kIOMapInhibitCache);
    memory->release();
    if(!*map)return kIOReturnNoMemory;
    *virtual_address = reinterpret_cast<volatile uint8_t *>((*map)->getVirtualAddress());
    return *virtual_address ? kIOReturnSuccess : kIOReturnNoMemory;
}

} // namespace

class NativeMaliGpuClient;

class NativeMaliGpuService final : public IOService {
    OSDeclareDefaultStructors(NativeMaliGpuService)

    NativeMaliService device_;
    NativeMaliXnuWindow gpu_window_;
    NativeMaliXnuWindow spm_window_;
    NativeMaliXnuWindow infracfg_window_;
    NativeMaliXnuWindow topckgen_window_;
    NativeMaliXnuWindow mfgcfg_window_;
    NativeMaliXnuWindow pwrap_window_;
    IOMemoryMap *gpu_map_ = nullptr;
    IOMemoryMap *spm_map_ = nullptr;
    IOMemoryMap *infracfg_map_ = nullptr;
    IOMemoryMap *topckgen_map_ = nullptr;
    IOMemoryMap *mfgcfg_map_ = nullptr;
    IOMemoryMap *pwrap_map_ = nullptr;
    NativeMaliGpuClient *clients_[NativeMaliServiceMaxClients]{};
    IOLock *lock_ = nullptr;
    bool ready_ = false;

    void releaseWindows();

public:
    bool init(OSDictionary *dictionary = nullptr) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
    IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
        OSDictionary *properties, IOUserClient **handler) APPLE_KEXT_OVERRIDE;

    /* Used by the user client. All of them except observation() expect the
     * caller to hold sessionLock(). */
    IOLock *sessionLock() const { return lock_; }
    NativeMaliService &device() { return device_; }
    const NativeMaliGpuObservation &observation() const
    {
        return device_.observation();
    }
    bool ready() const { return ready_; }
    bool attachSession(NativeMaliGpuClient *client);
    void detachSession(NativeMaliGpuClient *client);
};
OSDefineMetaClassAndStructors(NativeMaliGpuService, IOService)

/* One connection. It owns the pure client and everything the client needs:
 * its page tables, its buffers and its cache maintainer. A teardown that fails
 * keeps the Session (and therefore every pointer the pure service stores)
 * alive on purpose; free() then leaks it instead of deleting memory the GPU may
 * still reference. That is the same rule NativeAdrenoPublish.cpp applies to a
 * core whose teardown failed. */
class NativeMaliGpuClient final : public IOUserClient {
    OSDeclareDefaultStructors(NativeMaliGpuClient)
    friend class NativeMaliGpuService;

    /* Declaration order is destruction order reversed: core refers to vm and
     * the buffer source, vm refers to the page source, so core must die first
     * and the sources last. closeClient() normally empties all of them before
     * delete; this ordering only matters if a Session is ever deleted while
     * still prepared. */
    struct Session {
        NativeMaliXnuCache cache;
        NativeMaliXnuPages pages;
        NativeMaliXnuBuffers buffers;
        NativeMaliVm vm;
        NativeMaliUserClient core;
    };
    /* One user view per BO token, cached for clientMemoryForType() the way
     * NativeAdrenoPublish.cpp explains: IOConnectUnmapMemory64 calls
     * clientMemoryForType again and only unmaps a mapping of the same
     * descriptor. Tokens are the BO table's slot index + 1.
     * UNVERIFIED(XNU): that unmap behaviour is taken from the reference's
     * panic record, not reproduced here (docs/UNVERIFIED.md). */
    struct View {
        uint32_t token = 0;
        IOMemoryDescriptor *descriptor = nullptr;
    };

    NativeMaliGpuService *service_ = nullptr;
    Session *session_ = nullptr;
    View views_[NativeMaliBoTableCapacity]{};
    bool registered_ = false;

    static IOReturn action(OSObject *target, void *reference,
        IOExternalMethodArguments *arguments);
    IOReturn shutdownLocked();
    void afterMethod(uint32_t method, const void *input, const void *output);
    void dropViews();
    void dropDeadViews();
    View *viewSlot(uint32_t token);

public:
    void free() APPLE_KEXT_OVERRIDE;
    bool initWithTask(task_t owningTask, void *securityID, UInt32 type,
        OSDictionary *properties) APPLE_KEXT_OVERRIDE;
    IOReturn clientClose() APPLE_KEXT_OVERRIDE;
    IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options,
        IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;
    IOReturn externalMethod(uint32_t selector,
        IOExternalMethodArguments *arguments, IOExternalMethodDispatch *dispatch,
        OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;

    IOReturn mount(NativeMaliGpuService *service);
    IOReturn shutdown();
};
OSDefineMetaClassAndStructors(NativeMaliGpuClient, IOUserClient)

/* --- the service ------------------------------------------------------- */

bool NativeMaliGpuService::init(OSDictionary *dictionary)
{
    if(!IOService::init(dictionary))return false;
    lock_ = IOLockAlloc();
    return lock_ != nullptr;
}

void NativeMaliGpuService::free()
{
    releaseWindows();
    if(lock_) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
    IOService::free();
}

void NativeMaliGpuService::releaseWindows()
{
    /* The window objects must stop naming the virtual range before the maps go
     * away; a later start() binds them again. */
    gpu_window_.bind(nullptr, 0);
    spm_window_.bind(nullptr, 0);
    infracfg_window_.bind(nullptr, 0);
    topckgen_window_.bind(nullptr, 0);
    mfgcfg_window_.bind(nullptr, 0);
    pwrap_window_.bind(nullptr, 0);
    if(gpu_map_) { gpu_map_->release(); gpu_map_ = nullptr; }
    if(spm_map_) { spm_map_->release(); spm_map_ = nullptr; }
    if(infracfg_map_) { infracfg_map_->release(); infracfg_map_ = nullptr; }
    if(topckgen_map_) { topckgen_map_->release(); topckgen_map_ = nullptr; }
    if(mfgcfg_map_) { mfgcfg_map_->release(); mfgcfg_map_ = nullptr; }
    if(pwrap_map_) { pwrap_map_->release(); pwrap_map_ = nullptr; }
}

IOService *NativeMaliGpuService::probe(IOService *provider, SInt32 *score)
{
    if(!advertisesMali(provider))return nullptr;
    if(score)*score = 1000;
    return this;
}

bool NativeMaliGpuService::start(IOService *provider)
{
    if(!IOService::start(provider))return false;
    if(!provider || !lock_) {
        IOLog("NativeMali: no provider or no session lock\n");
        stop(provider);
        return false;
    }

    OSData *reg = OSDynamicCast(OSData, provider->getProperty("reg"));
    NativeMaliWindow windows[2]{};
    size_t count = 0;
    const uint32_t address_cells = cellProperty(provider, "#address-cells", 2);
    const uint32_t size_cells = cellProperty(provider, "#size-cells", 2);
    if(!reg || NativeMaliBoard::parseReg(
        static_cast<const uint8_t *>(reg->getBytesNoCopy()), reg->getLength(),
        address_cells, size_cells, windows, 2, &count) != kIOReturnSuccess ||
        count < 1 || windows[0].base != kGpuBase || windows[0].bytes < kGpuBytes) {
        IOLog("NativeMali: provider reg is not the 0x%llx window\n",
            static_cast<unsigned long long>(kGpuBase));
        stop(provider);
        return false;
    }

    IODeviceMemory *gpu_memory = getDeviceMemoryWithIndex(0);
    if(!gpu_memory) {
        IOLog("NativeMali: provider has no GPU device memory\n");
        stop(provider);
        return false;
    }
    gpu_map_ = gpu_memory->map(kIOMapAnywhere | kIOMapInhibitCache);
    if(!gpu_map_) {
        IOLog("NativeMali: GPU window did not map\n");
        stop(provider);
        return false;
    }
    /* Bound by the mapped length, not the `reg` length: a register target must
     * never point past the range it was handed. If the map is shorter than the
     * window the pure layers need, refuse rather than alias the next block. */
    if(gpu_map_->getLength() < kGpuBytes) {
        IOLog("NativeMali: GPU map is %llu bytes, need 0x%llx\n",
            static_cast<unsigned long long>(gpu_map_->getLength()),
            static_cast<unsigned long long>(kGpuBytes));
        stop(provider);
        return false;
    }
    gpu_window_.bind(
        reinterpret_cast<volatile uint8_t *>(gpu_map_->getVirtualAddress()),
        static_cast<size_t>(gpu_map_->getLength()));
    if(!gpu_window_.bound()) {
        IOLog("NativeMali: GPU window has no virtual address\n");
        stop(provider);
        return false;
    }

    volatile uint8_t *base = nullptr;
    if(mapFixedWindow(kSpmBase, kPowerWindowBytes, &spm_map_, &base) !=
        kIOReturnSuccess) {
        IOLog("NativeMali: SPM window did not map\n");
        stop(provider);
        return false;
    }
    spm_window_.bind(base, kPowerWindowBytes);
    if(mapFixedWindow(kInfracfgBase, kPowerWindowBytes, &infracfg_map_, &base) !=
        kIOReturnSuccess) {
        IOLog("NativeMali: INFRACFG window did not map\n");
        stop(provider);
        return false;
    }
    infracfg_window_.bind(base, kPowerWindowBytes);
    if(mapFixedWindow(kTopckgenBase, kPowerWindowBytes, &topckgen_map_, &base) !=
        kIOReturnSuccess) {
        IOLog("NativeMali: topckgen window did not map\n");
        stop(provider);
        return false;
    }
    topckgen_window_.bind(base, kPowerWindowBytes);
    if(mapFixedWindow(kMfgcfgBase, kPowerWindowBytes, &mfgcfg_map_, &base) !=
        kIOReturnSuccess) {
        IOLog("NativeMali: mfgcfg window did not map\n");
        stop(provider);
        return false;
    }
    mfgcfg_window_.bind(base, kPowerWindowBytes);
    if(mapFixedWindow(kPwrapBase, kPowerWindowBytes, &pwrap_map_, &base) !=
        kIOReturnSuccess) {
        IOLog("NativeMali: pwrap window did not map\n");
        stop(provider);
        return false;
    }
    pwrap_window_.bind(base, kPowerWindowBytes);

    NativeMaliBringupBlocks blocks{};
    blocks.power.spm = &spm_window_;
    blocks.power.infracfg = &infracfg_window_;
    blocks.power.topckgen = &topckgen_window_;
    blocks.power.mfgcfg = &mfgcfg_window_;
    blocks.power.pwrap = &pwrap_window_;
    blocks.gpu = &gpu_window_;
    IOReturn status = device_.prepare(blocks);
    if(status != kIOReturnSuccess) {
        IOLog("NativeMali: prepare failed 0x%x\n", status);
        stop(provider);
        return false;
    }

    NativeMaliBringupState state{};
    IOReturn teardown = kIOReturnSuccess;
    status = device_.start(&state, &teardown);
    /* Log the read-only probe output even when a later step refused, so the
     * first run on the phone (writes compiled out) has numbers to accept. */
    IOLog("NativeMali: probe spm=%08x/%08x protect=%08x/%08x clk=%08x/%08x gate=%08x vgpu=%u/%u vsram=%u/%u gpu_id=%08x ias=%u oas=%u\n",
        state.power.spmStatus, state.power.spmStatus2,
        state.power.infraProtectEnable, state.power.infraProtectStatus1,
        state.power.topMfgClockConfig, state.power.topClockUpdate,
        state.power.mfgClockGateStatus,
        state.power.vgpu.enabled, state.power.vgpu.microvolts,
        state.power.vsram.enabled, state.power.vsram.microvolts,
        state.gpu.gpuId, state.gpu.ias, state.gpu.oas);
    if(status != kIOReturnSuccess) {
        if(status == kIOReturnUnsupported)
            IOLog("NativeMali: GPU not brought up: writes are compiled out (NATIVE_MALI_M2_WRITES/M3_WRITES)\n");
        else
            IOLog("NativeMali: bring-up stopped 0x%x (teardown 0x%x)\n",
                status, teardown);
        stop(provider);
        return false;
    }
    if(teardown != kIOReturnSuccess)
        IOLog("NativeMali: bring-up rollback reported 0x%x\n", teardown);

    ready_ = true;
    registerService();
    IOLog("NativeMali: ready, publishing\n");
    return true;
}

void NativeMaliGpuService::stop(IOService *provider)
{
    if(!lock_) {
        IOService::stop(provider);
        return;
    }
    IOLockLock(lock_);
    ready_ = false;
    for(auto &slot : clients_)
        if(slot) {
            const IOReturn status = slot->shutdownLocked();
            if(status != kIOReturnSuccess)
                IOLog("NativeMali: a client did not close at stop (0x%x)\n", status);
        }
    const size_t left = device_.clients();
    IOLockUnlock(lock_);

    if(left != 0) {
        /* A session that could not release its address space still owns pages
         * the GPU may be walking. Powering down now would free them under the
         * hardware, so the block is left up and the next stop/start is the
         * documented recovery (docs/OPEN_QUESTIONS.md). */
        IOLog("NativeMali: %lu clients still open, not powering down\n",
            static_cast<unsigned long>(left));
        IOService::stop(provider);
        return;
    }

    IOReturn teardown = kIOReturnSuccess;
    IOReturn status = kIOReturnSuccess;
    /* start() may have failed before bring-up began (bad DT, no map); then
     * there is nothing to power down and device_.stop() would only report
     * NotReady. */
    if(device_.started()) {
        status = device_.stop(&teardown);
        if(status != kIOReturnSuccess)
            IOLog("NativeMali: power down failed 0x%x\n", status);
        if(teardown != kIOReturnSuccess)
            IOLog("NativeMali: power-down rollback reported 0x%x\n", teardown);
    }
    if(device_.started()) {
        /* A failed stop leaves the GPU powered. Releasing the mappings now
         * would remove the only way to finish the teardown; keep them and
         * report the failure (docs/OPEN_QUESTIONS.md: stop/start recovery). */
        IOLog("NativeMali: GPU still up after stop; keeping windows mapped\n");
        IOService::stop(provider);
        return;
    }
    releaseWindows();
    IOService::stop(provider);
}

bool NativeMaliGpuService::attachSession(NativeMaliGpuClient *client)
{
    for(auto &slot : clients_)
        if(!slot) {
            slot = client;
            return true;
        }
    return false;
}

void NativeMaliGpuService::detachSession(NativeMaliGpuClient *client)
{
    for(auto &slot : clients_)
        if(slot == client)slot = nullptr;
}

IOReturn NativeMaliGpuService::newUserClient(task_t owningTask, void *securityID,
    UInt32 type, OSDictionary *properties, IOUserClient **handler)
{
    if(handler)*handler = nullptr;
    if(!lock_)return kIOReturnNotReady;
    NativeMaliGpuClient *client = new NativeMaliGpuClient;
    if(!client)return kIOReturnNoMemory;
    if(!client->initWithTask(owningTask, securityID, type, properties)) {
        client->release();
        return kIOReturnNoMemory;
    }
    /* An override of newUserClient owns the whole open path: the client must be
     * attached to this service and started before it is returned, or it has no
     * provider chain (NativeAdrenoPublish.cpp's newUserClient does the same).
     * UNVERIFIED(XNU): whether the caller of newUserClient expects the driver
     * to do this, or does it itself, has not been observed on this kernel
     * (docs/UNVERIFIED.md). */
    if(!client->attach(this)) {
        IOLog("NativeMali: client attach failed\n");
        client->release();
        return kIOReturnNoResources;
    }
    if(!client->start(this)) {
        IOLog("NativeMali: client start failed\n");
        client->detach(this);
        client->release();
        return kIOReturnNoResources;
    }
    IOLockLock(lock_);
    if(!ready_) {
        IOLockUnlock(lock_);
        client->terminate();
        client->release();
        return kIOReturnNotReady;
    }
    const IOReturn status = client->mount(this);
    IOLockUnlock(lock_);
    if(status != kIOReturnSuccess) {
        client->terminate();
        client->release();
        return status;
    }
    if(handler)*handler = client;
    return kIOReturnSuccess;
}

/* --- the user client --------------------------------------------------- */

bool NativeMaliGpuClient::initWithTask(task_t owningTask, void *securityID,
    UInt32 type, OSDictionary *properties)
{
    if(!IOUserClient::initWithTask(owningTask, securityID, type, properties))
        return false;
    return true;
}

IOReturn NativeMaliGpuClient::mount(NativeMaliGpuService *service)
{
    if(!service || session_)return kIOReturnBadArgument;
    Session *session = new Session;
    if(!session)return kIOReturnNoMemory;
    service_ = service;
    const uint32_t oas = service->observation().oas;
    session->pages.setOutputLimit(oas);
    session->buffers.setOutputLimit(oas);
    /* The phone's DT has no "dma-coherent" property, so both the walker and the
     * buffers are non-coherent (docs/DEVICE.md, NativeMaliMemory.h). The
     * maintainer is required by NativeMaliMemory::prepare() in that case. */
    const IOReturn status = service->device().openClient(&session->core,
        &session->vm, &session->pages, &session->buffers, &session->cache,
        false, NATIVE_MALI_ABI_MAX_BO_BYTES);
    if(status != kIOReturnSuccess) {
        IOLog("NativeMali: openClient failed 0x%x\n", status);
        delete session;
        service_ = nullptr;
        return status;
    }
    if(!service->attachSession(this)) {
        /* openClient() and the session table have the same capacity and are
         * changed under the same lock, so this should be unreachable. If
         * closeClient() also fails, the Session is kept alive rather than
         * deleted: the service still holds a pointer into it. */
        const IOReturn closed = service->device().closeClient(&session->core);
        if(closed != kIOReturnSuccess) {
            IOLog("NativeMali: no session slot and closeClient failed 0x%x; session leaks\n",
                closed);
            session_ = session;
            return closed;
        }
        delete session;
        service_ = nullptr;
        return kIOReturnNoResources;
    }
    session_ = session;
    registered_ = true;
    return kIOReturnSuccess;
}

IOReturn NativeMaliGpuClient::shutdownLocked()
{
    if(!registered_ || !session_ || !service_)return kIOReturnSuccess;
    dropViews();
    const IOReturn status = service_->device().closeClient(&session_->core);
    if(status != kIOReturnSuccess) {
        /* Keep the Session and the registration: the pure service still holds
         * these pointers and the address space, and closeClient() is written
         * to be retried. */
        IOLog("NativeMali: closeClient failed 0x%x, session kept\n", status);
        return status;
    }
    delete session_;
    session_ = nullptr;
    registered_ = false;
    service_->detachSession(this);
    return kIOReturnSuccess;
}

IOReturn NativeMaliGpuClient::shutdown()
{
    if(!service_)return kIOReturnNotReady;
    IOLockLock(service_->sessionLock());
    const IOReturn status = shutdownLocked();
    IOLockUnlock(service_->sessionLock());
    return status;
}

IOReturn NativeMaliGpuClient::clientClose()
{
    const IOReturn status = shutdown();
    if(status != kIOReturnSuccess)return status;
    terminate();
    return kIOReturnSuccess;
}

void NativeMaliGpuClient::free()
{
    /* A failed teardown leaves session_ and registered_ set on purpose; the
     * pure service still points into that Session. Leaking it keeps those
     * pointers valid until stop()/start(); deleting it here would turn the next
     * register access into use-after-free.
     * UNVERIFIED(XNU): this takes the service lock, so a synchronous free() from
     * inside stop() (which holds it) would deadlock; the call order has not been
     * observed (docs/UNVERIFIED.md). */
    if(service_) {
        IOLockLock(service_->sessionLock());
        dropViews();
        if(session_)
            IOLog("NativeMali: freeing a client whose teardown failed; its session leaks\n");
        service_->detachSession(this);
        IOLockUnlock(service_->sessionLock());
    }
    session_ = nullptr;
    IOUserClient::free();
}

NativeMaliGpuClient::View *NativeMaliGpuClient::viewSlot(uint32_t token)
{
    if(token == 0 || token > NativeMaliBoTableCapacity)return nullptr;
    return &views_[token - 1];
}

void NativeMaliGpuClient::dropViews()
{
    for(auto &view : views_)
        if(view.descriptor) {
            view.descriptor->release();
            view = View{};
        }
}

void NativeMaliGpuClient::dropDeadViews()
{
    /* After a BoFree the freed token no longer resolves; every other live
     * mapping keeps its view so a later IOConnectUnmapMemory64 still sees the
     * same descriptor. */
    if(!session_)return;
    for(auto &view : views_)
        if(view.descriptor) {
            if(session_->core.memoryForToken(view.token))continue;
            view.descriptor->release();
            view = View{};
        }
}

IOReturn NativeMaliGpuClient::clientMemoryForType(UInt32 type,
    IOOptionBits *options, IOMemoryDescriptor **memory)
{
    if(memory)*memory = nullptr;
    if(!memory || !options)return kIOReturnBadArgument;
    if(!service_)return kIOReturnNotReady;
    IOLockLock(service_->sessionLock());
    /* Re-read under the lock: clientClose() can delete the Session while this
     * call was waiting for it. */
    Session *session = session_;
    if(!session) {
        IOLockUnlock(service_->sessionLock());
        return kIOReturnNotReady;
    }
    IOReturn status = kIOReturnNotFound;
    View *view = viewSlot(type);
    if(view && view->descriptor && view->token == type) {
        view->descriptor->retain();
        *memory = view->descriptor;
        *options = kIOMapAnywhere;
        status = kIOReturnSuccess;
    } else if(view) {
        NativeMaliMemory *backing = session->core.memoryForToken(type);
        IOMemoryDescriptor *descriptor =
            backing ? session->buffers.descriptor(backing->cpu()) : nullptr;
        if(descriptor) {
            view->token = type;
            view->descriptor = descriptor;   /* already retained by the source */
            *memory = descriptor;
            *options = kIOMapAnywhere;
            status = kIOReturnSuccess;
        } else {
            status = kIOReturnBadArgument;
        }
    }
    IOLockUnlock(service_->sessionLock());
    return status;
}

void NativeMaliGpuClient::afterMethod(uint32_t method, const void *input,
    const void *output)
{
    if(!session_)return;
    if(method == NativeMaliMethodBoCpuUnmap && input) {
        const NativeMaliAbiBoCpuUnmap *request =
            static_cast<const NativeMaliAbiBoCpuUnmap *>(input);
        NativeMaliMemory *memory = session_->core.memoryForToken(request->token);
        /* The CPU is done writing this BO: write the cache back before the GPU
         * reads it. UNVERIFIED(HW): the clean-on-unmap/invalidate-on-map policy
         * has not been tested for coherency on this phone (docs/UNVERIFIED.md). */
        if(memory) {
            const IOReturn status = memory->cleanForDevice(0, memory->bytes());
            if(status != kIOReturnSuccess)
                IOLog("NativeMali: clean before unmap failed 0x%x\n", status);
        }
        /* The view is kept until BoFree: IOConnectUnmapMemory64 asks
         * clientMemoryForType for the same descriptor and only unmaps that
         * one, and this repository has no equivalent of the reference's
         * removeMappingForDescriptor() to force the user mapping down first.
         * Dropping the view here would make a later unmap a no-op. The client
         * contract is to unmap the user range before BoFree; docs/UNVERIFIED.md
         * records that this is not enforced. */
    } else if(method == NativeMaliMethodBoCpuMap && output) {
        const NativeMaliAbiBoCpuMapReply *reply =
            static_cast<const NativeMaliAbiBoCpuMapReply *>(output);
        NativeMaliMemory *memory = session_->core.memoryForToken(reply->token);
        /* Drop stale CPU lines so a read sees what the GPU last wrote. */
        if(memory) {
            const IOReturn status = memory->invalidateForCpu(0, memory->bytes());
            if(status != kIOReturnSuccess)
                IOLog("NativeMali: invalidate on map failed 0x%x\n", status);
        }
    } else if(method == NativeMaliMethodBoFree) {
        /* The handle is gone; drop the views whose tokens no longer resolve,
         * and leave every still-mapped BO alone. */
        dropDeadViews();
    }
}

namespace {

/* IOExternalMethodDispatch wants exact sizes. They come from the same table the
 * pure dispatch checks, so the two cannot drift; a selector outside the table
 * is refused before IOKit copies anything.
 * UNVERIFIED(XNU): the dispatch entry lives on the stack for one call; whether
 * externalMethod() may keep or re-read it is not documented here, and the
 * structure-size field width has not been compiled (docs/UNVERIFIED.md). */
IOReturn allocateDispatch(uint32_t selector, IOExternalMethodDispatch *entry,
    IOExternalMethodAction action)
{
    if(!entry || selector < NativeMaliMethodQuery || selector > NativeMaliMethodLast)
        return kIOReturnUnsupported;
    const size_t input = NativeMaliUserClient::inputSize(selector);
    const size_t output = NativeMaliUserClient::outputSize(selector);
    if(input == NativeMaliUserClient::kUnsupportedSize ||
        output == NativeMaliUserClient::kUnsupportedSize)
        return kIOReturnUnsupported;
    entry->action = action;
    entry->checkScalarInputCount = 0;
    entry->checkStructureInputSize = static_cast<uint32_t>(input);
    entry->checkScalarOutputCount = 0;
    entry->checkStructureOutputSize = static_cast<uint32_t>(output);
    return kIOReturnSuccess;
}

} // namespace

IOReturn NativeMaliGpuClient::action(OSObject *target, void *,
    IOExternalMethodArguments *arguments)
{
    NativeMaliGpuClient *client = OSDynamicCast(NativeMaliGpuClient, target);
    if(!client || !arguments)return kIOReturnBadArgument;
    NativeMaliGpuService *service = client->service_;
    if(!service)return kIOReturnNotReady;

    IOLockLock(service->sessionLock());
    /* session_ is re-read under the lock: clientClose() can delete it while
     * this call waits. */
    Session *session = client->session_;
    IOReturn status = kIOReturnNotReady;
    if(session && service->device().clientUsable(&session->core))
        status = session->core.dispatch(arguments->selector,
            arguments->structureInput, arguments->structureInputSize,
            arguments->structureOutput, arguments->structureOutputSize);
    if(status == kIOReturnSuccess)
        client->afterMethod(arguments->selector, arguments->structureInput,
            arguments->structureOutput);
    IOLockUnlock(service->sessionLock());
    return status;
}

IOReturn NativeMaliGpuClient::externalMethod(uint32_t selector,
    IOExternalMethodArguments *arguments, IOExternalMethodDispatch *,
    OSObject *, void *)
{
    if(!arguments)return kIOReturnBadArgument;
    IOExternalMethodDispatch entry{};
    /* Passing the static action from here, not from the free helper, keeps
     * access to the private member. */
    const IOReturn status =
        allocateDispatch(selector, &entry, &NativeMaliGpuClient::action);
    if(status != kIOReturnSuccess)return status;
    return IOUserClient::externalMethod(selector, arguments, &entry, this, nullptr);
}
