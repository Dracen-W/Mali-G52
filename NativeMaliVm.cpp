/* SPDX-License-Identifier: GPL-2.0-only */
/* Descriptor encoding and table walking for ARM_MALI_LPAE; see NativeMaliVm.h
 * for every fact and where it comes from. Nothing here touches hardware. */
#include "NativeMaliVm.h"
#include <string.h>

namespace {

constexpr uint64_t kTypeMask = 0x3;
constexpr uint64_t kTypeBlock = 1;   /* leaf, at every level for ARM_MALI_LPAE */
constexpr uint64_t kTypeTable = 3;
constexpr uint64_t kAddressMask = 0x0000fffffffff000ull;   /* [47:12] */
constexpr uint32_t kMaxLevels = 4;
constexpr uint32_t kGranuleShift = 12;

constexpr uint64_t kHapRead = 1ull << 6;
constexpr uint64_t kHapWrite = 2ull << 6;
constexpr uint64_t kAttrIndexShift = 2;
constexpr uint64_t kAttrIndexCache = 1;
constexpr uint64_t kAttrIndexDevice = 2;
constexpr uint64_t kShareOuter = 2ull << 8;
constexpr uint64_t kExecuteNever = 3ull << 53;

/* Address mode 3 (table) comes with encodeTable()'s type field, so the only
 * extra TRANSTAB bits are these two. */
constexpr uint64_t kTtbrReadInner = 1ull << 2;
constexpr uint64_t kTtbrShareOuter = 1ull << 4;
constexpr uint64_t kMemattrImplementationDefined = 0x88ull;
constexpr uint64_t kMemattrWriteAllocate = 0x8dull;

} // namespace

size_t NativeMaliVm::physHashSlot(uint64_t phys)
{
    const uint64_t pageNum = phys >> kGranuleShift;
    return static_cast<size_t>((pageNum * UINT64_C(0x9e3779b97f4a7c15)) >> 40) &
        kPhysHashMask;
}

void NativeMaliVm::indexTablePhys(uint64_t phys, size_t tableIndex)
{
    size_t slot = physHashSlot(phys);
    for(size_t n = 0; n < kPhysHashSize; ++n, slot = (slot + 1) & kPhysHashMask) {
        if(!physHash_[slot]) {
            physHash_[slot] = static_cast<uint16_t>(tableIndex + 1);
            return;
        }
    }
}

bool NativeMaliVm::lookupTablePhys(uint64_t phys, uint64_t **cpuOut,
    size_t *indexOut) const
{
    size_t slot = physHashSlot(phys);
    for(size_t n = 0; n < kPhysHashSize; ++n, slot = (slot + 1) & kPhysHashMask) {
        const uint16_t encoded = physHash_[slot];
        if(!encoded)return false;
        const size_t idx = static_cast<size_t>(encoded - 1);
        if(idx < tables_ && page_[idx].phys == phys) {
            if(cpuOut)*cpuOut = static_cast<uint64_t *>(page_[idx].cpu);
            if(indexOut)*indexOut = idx;
            return true;
        }
    }
    return false;
}

uint64_t NativeMaliVm::encodePhysical(uint64_t paddr)
{
    /* paddr_to_iopte(): the overlapping RES0 bits are folded out, so a 40-bit
     * address (the Mali maximum) comes out unchanged. */
    return (paddr | (paddr >> (48 - 12))) & kAddressMask;
}

uint64_t NativeMaliVm::decodePhysical(uint64_t pte)
{
    /* Granule is 4 KiB, so no packed high-order bits to rotate back. */
    return pte & kAddressMask;
}

uint64_t NativeMaliVm::encodeLeaf(uint64_t paddr, uint32_t prot)
{
    uint64_t pte = kTypeBlock;
    if(prot & NativeMaliVmRead)pte |= kHapRead;
    if(prot & NativeMaliVmWrite)pte |= kHapWrite;
    if(prot & NativeMaliVmMmio)pte |= kAttrIndexDevice << kAttrIndexShift;
    else if(prot & NativeMaliVmCached)pte |= kAttrIndexCache << kAttrIndexShift;
    /* Always outer-shareable: the source only chooses inner-shareable for the
     * generic formats, and this is ARM_MALI_LPAE. */
    pte |= kShareOuter;
    if(prot & NativeMaliVmNoExec)pte |= kExecuteNever;
    /* No AF and no nG bit: both are skipped for ARM_MALI_LPAE. */
    return pte | encodePhysical(paddr);
}

uint64_t NativeMaliVm::encodeTable(uint64_t table_phys)
{
    return encodePhysical(table_phys) | kTypeTable;
}

NativeMaliVmGeometry NativeMaliVm::geometry(uint32_t ias, uint32_t oas)
{
    NativeMaliVmGeometry g{};
    g.ias = ias;
    g.oas = oas;
    g.bits_per_level = NativeMaliVmBitsPerLevel;
    const uint32_t va_bits = ias > kGranuleShift ? ias - kGranuleShift : 0;
    uint32_t levels = (va_bits + g.bits_per_level - 1) / g.bits_per_level;
    if(levels < 1)levels = 1;
    if(levels > kMaxLevels)levels = kMaxLevels;
    g.start_level = kMaxLevels - levels;
    g.pgd_bits = va_bits - g.bits_per_level * (levels - 1);
    /* "Mali seems to need a full 4-level table regardless of IAS": a walk that
     * would start below level 0 starts at level 0 with one valid entry. */
    if(g.start_level > 0) {
        g.start_level = 0;
        g.pgd_bits = 0;
    }
    g.levels = kMaxLevels - g.start_level;
    g.span = 1ull << (kGranuleShift + g.bits_per_level * (g.levels - 1) + g.pgd_bits);
    return g;
}

uint32_t NativeMaliVm::shift(uint32_t level) const
{
    return (kMaxLevels - level) * geometry_.bits_per_level + 3;
}

uint32_t NativeMaliVm::indexBits(uint32_t level) const
{
    return level == geometry_.start_level ? geometry_.pgd_bits : geometry_.bits_per_level;
}

uint32_t NativeMaliVm::index(uint64_t iova, uint32_t level) const
{
    const uint32_t bits = indexBits(level);
    if(!bits)return 0;
    return uint32_t((iova >> shift(level)) & ((UINT64_C(1) << bits) - 1));
}

void NativeMaliVm::markDirty(size_t tableIndex)
{
    if(tableIndex < NativeMaliVmMaxTables)
        dirtyTables_[tableIndex >> 6] |= UINT64_C(1) << (tableIndex & 63u);
}

IOReturn NativeMaliVm::allocate(uint64_t **table, uint64_t *phys,
    size_t *tableIndex)
{
    if(tables_ >= NativeMaliVmMaxTables || !source_)return kIOReturnNoResources;
    NativeMaliPage page{};
    const IOReturn result = source_->alloc(&page);
    if(result != kIOReturnSuccess)return result;
    if(!page.cpu || (page.phys & (NativeMaliVmGranule - 1)) ||
        page.bytes < NativeMaliVmGranule) {
        /* A source that cannot honour the contract is not quietly trusted. */
        source_->release(page);
        return kIOReturnBadArgument;
    }
    memset(page.cpu, 0, NativeMaliVmGranule);
    const size_t idx = tables_;
    page_[idx] = page;
    ++tables_;
    indexTablePhys(page.phys, idx);
    markDirty(idx);
    *table = static_cast<uint64_t *>(page.cpu);
    *phys = page.phys;
    if(tableIndex)*tableIndex = idx;
    return kIOReturnSuccess;
}

IOReturn NativeMaliVm::descendL3(uint64_t iova, uint64_t **l3Table,
    size_t *tableIndex)
{
    uint64_t *table = pgd_;
    size_t currentIndex = 0;
    for(uint32_t level = 0; level < kMaxLevels - 1; ++level) {
        uint64_t *slot = &table[index(iova, level)];
        const uint64_t entry = *slot;
        if(!entry) {
            uint64_t *next = nullptr;
            uint64_t phys = 0;
            size_t nextIndex = 0;
            const IOReturn result = allocate(&next, &phys, &nextIndex);
            if(result != kIOReturnSuccess)return result;
            *slot = encodeTable(phys);
            markDirty(currentIndex);
            table = next;
            currentIndex = nextIndex;
        } else if((entry & kTypeMask) == kTypeTable) {
            uint64_t *next = nullptr;
            size_t nextIndex = 0;
            const uint64_t targetPhys = decodePhysical(entry);
            if(!lookupTablePhys(targetPhys, &next, &nextIndex))
                return kIOReturnIOError;   /* a table this space never made */
            table = next;
            currentIndex = nextIndex;
        } else {
            /* A leaf blocks the way down: mapping over it would leak it. */
            return kIOReturnBusy;
        }
    }
    *l3Table = table;
    if(tableIndex)*tableIndex = currentIndex;
    return kIOReturnSuccess;
}

/* The read-only twin of descendL3(): it never allocates. */
IOReturn NativeMaliVm::findL3(uint64_t iova, uint64_t **l3Table,
    size_t *tableIndex) const
{
    uint64_t *table = pgd_;
    size_t currentIndex = 0;
    for(uint32_t level = 0; level < kMaxLevels - 1; ++level) {
        const uint64_t entry = table[index(iova, level)];
        if(!entry) {
            *l3Table = nullptr;
            return kIOReturnSuccess;
        }
        if((entry & kTypeMask) != kTypeTable) {
            *l3Table = nullptr;   /* a leaf above the last level: not ours */
            return kIOReturnSuccess;
        }
        uint64_t *next = nullptr;
        size_t nextIndex = 0;
        const uint64_t targetPhys = decodePhysical(entry);
        if(!lookupTablePhys(targetPhys, &next, &nextIndex))
            return kIOReturnIOError;
        table = next;
        currentIndex = nextIndex;
    }
    *l3Table = table;
    if(tableIndex)*tableIndex = currentIndex;
    return kIOReturnSuccess;
}

NativeMaliVm::~NativeMaliVm()
{
    reset();
}

IOReturn NativeMaliVm::prepare(NativeMaliPageSource *source, uint32_t ias, uint32_t oas,
    bool coherent_walk, NativeMaliCacheMaintainer *maintainer)
{
    if(prepared_)return kIOReturnBusy;
    if(!source)return kIOReturnBadArgument;
    if(ias < NativeMaliVmMinIas || ias > NativeMaliVmMaxIas)return kIOReturnUnsupported;
    if(oas < NativeMaliVmMinIas || oas > NativeMaliVmMaxOas)return kIOReturnUnsupported;
    source_ = source;
    maintainer_ = maintainer;
    coherent_ = coherent_walk;
    geometry_ = geometry(ias, oas);
    uint64_t *root = nullptr;
    uint64_t root_phys = 0;
    IOReturn result = allocate(&root, &root_phys, nullptr);
    if(result != kIOReturnSuccess) {
        source_ = nullptr;   /* nothing was allocated, so nothing to release */
        pgd_ = nullptr;
        return result;
    }
    pgd_ = root;
    prepared_ = true;
    /* io-pgtable-arm.c:1263: the empty PGD is made visible before TRANSTAB may
     * be written. With a maintainer that is a clean, otherwise the caller's
     * job. A failure here tears everything back down. */
    result = publishTables();
    if(result != kIOReturnSuccess) {
        reset();
        return result;
    }
    return kIOReturnSuccess;
}

IOReturn NativeMaliVm::publishTables()
{
    if(!prepared_ || coherent_ || !maintainer_)return kIOReturnSuccess;
    for(size_t word = 0; word < (NativeMaliVmMaxTables + 63) / 64; ++word) {
        uint64_t mask = dirtyTables_[word];
        while(mask) {
            const uint32_t bit = static_cast<uint32_t>(__builtin_ctzll(mask));
            const size_t idx = (word << 6) + bit;
            if(idx >= tables_) {
                dirtyTables_[word] &= ~(UINT64_C(1) << bit);
                mask &= mask - 1;
                continue;
            }
            const IOReturn result = maintainer_->maintain(page_[idx].cpu,
                NativeMaliVmGranule, NativeMaliCacheClean);
            if(result != kIOReturnSuccess)return result;
            dirtyTables_[word] &= ~(UINT64_C(1) << bit);
            mask &= mask - 1;
        }
    }
    return kIOReturnSuccess;
}

void NativeMaliVm::reset()
{
    if(source_)for(size_t i = 0; i < tables_; ++i)source_->release(page_[i]);
    memset(page_, 0, sizeof(page_));
    memset(physHash_, 0, sizeof(physHash_));
    memset(dirtyTables_, 0, sizeof(dirtyTables_));
    tables_ = 0;
    pgd_ = nullptr;
    source_ = nullptr;
    maintainer_ = nullptr;
    prepared_ = false;
    coherent_ = false;
    mapped_pages_ = 0;
    geometry_ = NativeMaliVmGeometry{};
}

uint64_t NativeMaliVm::transtab() const
{
    if(!prepared_)return 0;
    uint64_t value = encodeTable(page_[0].phys) | kTtbrReadInner;
    if(coherent_)value |= kTtbrShareOuter;
    return value;
}

uint64_t NativeMaliVm::memattr() const
{
    return (kMemattrImplementationDefined << 0) |
        (kMemattrWriteAllocate << 8) |
        (kMemattrImplementationDefined << 16);
}

/* Alignment, non-zero size, no wrap, inside the address space and inside the
 * table layout. The physical end is checked by map(), which is the only caller
 * that has one. */
static IOReturn checkRange(const NativeMaliVmGeometry &geometry, uint64_t iova,
    uint64_t bytes)
{
    if(!bytes || (iova & (NativeMaliVmGranule - 1)) ||
        (bytes & (NativeMaliVmGranule - 1)))
        return kIOReturnBadArgument;
    if(iova + bytes < iova)return kIOReturnBadArgument;
    if(iova + bytes > (UINT64_C(1) << geometry.ias))return kIOReturnBadArgument;
    if(iova + bytes > geometry.span)return kIOReturnBadArgument;
    return kIOReturnSuccess;
}

IOReturn NativeMaliVm::map(uint64_t iova, uint64_t paddr, uint64_t bytes, uint32_t prot)
{
    if(!prepared_)return kIOReturnNotReady;
    if(!(prot & (NativeMaliVmRead | NativeMaliVmWrite)))return kIOReturnBadArgument;
    if(prot & NativeMaliVmMmio && prot & NativeMaliVmCached)return kIOReturnBadArgument;
    if(paddr & (NativeMaliVmGranule - 1))return kIOReturnBadArgument;
    if(paddr + bytes < paddr)return kIOReturnBadArgument;
    if(paddr + bytes > (UINT64_C(1) << geometry_.oas))return kIOReturnBadArgument;
    IOReturn result = checkRange(geometry_, iova, bytes);
    if(result != kIOReturnSuccess)return result;
    /* Two passes with L3 batching (up to 512 leaves / 2 MiB per walk):
     * refuse to overwrite anything, then write. A refusal therefore leaves no
     * half-applied range behind. Tables created by the check stay allocated;
     * only reset() takes them back. */
    for(uint64_t offset = 0; offset < bytes; ) {
        uint64_t *l3 = nullptr;
        size_t tableIndex = 0;
        result = descendL3(iova + offset, &l3, &tableIndex);
        if(result != kIOReturnSuccess)return result;
        const uint32_t startSlot = index(iova + offset, kMaxLevels - 1);
        const uint64_t remaining = (bytes - offset) >> kGranuleShift;
        const uint64_t slotsLeft = NativeMaliVmPtesPerTable - startSlot;
        const uint64_t batch = remaining < slotsLeft ? remaining : slotsLeft;
        for(uint64_t i = 0; i < batch; ++i)
            if(l3[startSlot + i])return kIOReturnBusy;
        offset += batch << kGranuleShift;
    }
    for(uint64_t offset = 0; offset < bytes; ) {
        uint64_t *l3 = nullptr;
        size_t tableIndex = 0;
        result = descendL3(iova + offset, &l3, &tableIndex);
        if(result != kIOReturnSuccess)return result;
        const uint32_t startSlot = index(iova + offset, kMaxLevels - 1);
        const uint64_t remaining = (bytes - offset) >> kGranuleShift;
        const uint64_t slotsLeft = NativeMaliVmPtesPerTable - startSlot;
        const uint64_t batch = remaining < slotsLeft ? remaining : slotsLeft;
        for(uint64_t i = 0; i < batch; ++i) {
            l3[startSlot + i] =
                encodeLeaf(paddr + offset + (i << kGranuleShift), prot);
        }
        mapped_pages_ += batch;
        markDirty(tableIndex);
        offset += batch << kGranuleShift;
    }
    /* Publish only dirty tables (newly created tables + modified L3 tables)
     * before the caller flushes the MMU. A failed publish takes the range back
     * out and returns the maintainer's error unchanged. */
    result = publishTables();
    if(result != kIOReturnSuccess) {
        (void)unmap(iova, bytes);
        return result;
    }
    return kIOReturnSuccess;
}

IOReturn NativeMaliVm::unmap(uint64_t iova, uint64_t bytes)
{
    if(!prepared_)return kIOReturnNotReady;
    const IOReturn result = checkRange(geometry_, iova, bytes);
    if(result != kIOReturnSuccess)return result;
    for(uint64_t offset = 0; offset < bytes; ) {
        uint64_t *l3 = nullptr;
        size_t tableIndex = 0;
        const IOReturn walked = findL3(iova + offset, &l3, &tableIndex);
        if(walked != kIOReturnSuccess)return walked;
        const uint32_t startSlot = index(iova + offset, kMaxLevels - 1);
        const uint64_t remaining = (bytes - offset) >> kGranuleShift;
        const uint64_t slotsLeft = NativeMaliVmPtesPerTable - startSlot;
        const uint64_t batch = remaining < slotsLeft ? remaining : slotsLeft;
        if(l3) {
            bool changed = false;
            for(uint64_t i = 0; i < batch; ++i) {
                if(l3[startSlot + i]) {
                    l3[startSlot + i] = 0;
                    if(mapped_pages_)--mapped_pages_;
                    changed = true;
                }
            }
            if(changed)markDirty(tableIndex);
        }
        offset += batch << kGranuleShift;
    }
    /* The cleared entries must be visible to the walker before the caller
     * flushes the range; on failure the leaves are already clear, so repeating
     * unmap() only publishes again. */
    return publishTables();
}

IOReturn NativeMaliVm::descriptor(uint64_t iova, uint64_t *pte) const
{
    if(!pte)return kIOReturnBadArgument;
    *pte = 0;
    if(!prepared_)return kIOReturnNotReady;
    if(iova >= geometry_.span)return kIOReturnBadArgument;
    uint64_t *l3 = nullptr;
    const IOReturn walked = findL3(iova, &l3, nullptr);
    if(walked != kIOReturnSuccess)return walked;
    if(l3)*pte = l3[index(iova, kMaxLevels - 1)];
    return kIOReturnSuccess;
}
