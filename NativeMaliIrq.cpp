/* SPDX-License-Identifier: GPL-2.0-only */
/* The decoding itself; the bits and the line numbers are in NativeMaliIrq.h. */
#include "NativeMaliIrq.h"

NativeMaliIrq::Gpu NativeMaliIrq::decodeGpu(uint32_t status)
{
    Gpu gpu;
    gpu.status = status;
    gpu.unknown = status & ~static_cast<uint32_t>(GPU_IRQ_MASK_ALL);
    return gpu;
}

NativeMaliIrq::Jobs NativeMaliIrq::decodeJobs(uint32_t status, uint32_t slots)
{
    Jobs jobs;
    jobs.status = status;
    if(slots == 0 || slots > 16) {
        jobs.unknown = status;
        return jobs;
    }
    /* DONE is the low half and ERR the high half (panfrost_regs.h:263-264). */
    const uint32_t low = (1u << slots) - 1u;
    jobs.done = status & low;
    jobs.error = (status >> 16) & low;
    jobs.unknown = status & ~((low << 16) | low);
    return jobs;
}

NativeMaliIrq::Mmu NativeMaliIrq::decodeMmu(uint32_t status, uint32_t present)
{
    Mmu mmu;
    mmu.status = status;
    /* The walk in panfrost_mmu.c:889 pairs BIT(as) with BIT(as + 16). */
    const uint32_t low = status & 0xFFFFu;
    const uint32_t high = (status >> 16) & 0xFFFFu;
    const uint32_t asPresent = present & 0xFFFFu;
    mmu.fault = low & asPresent;
    mmu.second = high & asPresent;
    mmu.absent = (low | high) & ~asPresent;
    return mmu;
}

uint32_t NativeMaliIrq::mmuClearMask(uint32_t as)
{
    if(as >= 16)return 0;
    return (1u << as) | (1u << (as + 16));
}
