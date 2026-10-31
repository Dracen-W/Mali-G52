/* SPDX-License-Identifier: GPL-2.0-only */
/* NativeMaliIrq against the pinned handlers. Nothing here touches a register:
 * the point is that a status word decodes the way panfrost_gpu_irq_handler()
 * (panfrost_gpu.c:24-60), panfrost_jm_handle_irq() (panfrost_job.c:522-571) and
 * the MMU thread (panfrost_mmu.c:882-906) read it. */
#include "NativeMaliIrq.h"
#include "NativeMaliRegs.h"
#include <cstdio>
#include <cstdlib>

#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)

namespace {

/* panfrost_gpu.c:37: only these two make the handler treat the interrupt as an
 * error, read the fault address, log it and mask the line. */
void testGpuErrorPair()
{
    CHECK(GPU_IRQ_MASK_ERROR == (GPU_IRQ_FAULT | GPU_IRQ_MULTIPLE_FAULT));
    CHECK(GPU_IRQ_FAULT == BIT(0));
    CHECK(GPU_IRQ_MULTIPLE_FAULT == BIT(7));
}

void testGpuDecode()
{
    /* Nothing set: the handler returns IRQ_NONE (panfrost_gpu.c:34-35). */
    {
        const NativeMaliIrq::Gpu gpu = NativeMaliIrq::decodeGpu(0);
        CHECK(!gpu.any());
        CHECK(!gpu.error());
        CHECK(gpu.unknown == 0);
        CHECK(!gpu.fault() && !gpu.multipleFault() && !gpu.resetCompleted() &&
            !gpu.powerChanged() && !gpu.powerChangedAll() &&
            !gpu.perfcntSample() && !gpu.cleanCaches());
    }
    /* One bit at a time: every known source maps to exactly its own accessor,
     * and none of them is mistaken for an error except the pair above. */
    struct Case { uint32_t bit; bool error; };
    const Case cases[] = {
        {GPU_IRQ_FAULT, true},
        {GPU_IRQ_MULTIPLE_FAULT, true},
        {GPU_IRQ_RESET_COMPLETED, false},
        {GPU_IRQ_POWER_CHANGED, false},
        {GPU_IRQ_POWER_CHANGED_ALL, false},
        {GPU_IRQ_PERFCNT_SAMPLE_COMPLETED, false},
        {GPU_IRQ_CLEAN_CACHES_COMPLETED, false}
    };
    for(const Case &c : cases) {
        const NativeMaliIrq::Gpu gpu = NativeMaliIrq::decodeGpu(c.bit);
        CHECK(gpu.any());
        CHECK(gpu.status == c.bit);
        CHECK(gpu.unknown == 0);
        CHECK(gpu.error() == c.error);
        CHECK(gpu.fault() == (c.bit == GPU_IRQ_FAULT));
        CHECK(gpu.multipleFault() == (c.bit == GPU_IRQ_MULTIPLE_FAULT));
        CHECK(gpu.resetCompleted() == (c.bit == GPU_IRQ_RESET_COMPLETED));
        CHECK(gpu.powerChanged() == (c.bit == GPU_IRQ_POWER_CHANGED));
        CHECK(gpu.powerChangedAll() == (c.bit == GPU_IRQ_POWER_CHANGED_ALL));
        CHECK(gpu.perfcntSample() ==
            (c.bit == GPU_IRQ_PERFCNT_SAMPLE_COMPLETED));
        CHECK(gpu.cleanCaches() == (c.bit == GPU_IRQ_CLEAN_CACHES_COMPLETED));
    }
    /* Every source at once, the word the handler would clear in one write. */
    {
        const uint32_t all = GPU_IRQ_MASK_ALL;
        CHECK(all == 0x00030781u);
        const NativeMaliIrq::Gpu gpu = NativeMaliIrq::decodeGpu(all);
        CHECK(gpu.fault() && gpu.multipleFault() && gpu.resetCompleted() &&
            gpu.powerChanged() && gpu.powerChangedAll() && gpu.perfcntSample() &&
            gpu.cleanCaches());
        CHECK(gpu.error());
        CHECK(gpu.unknown == 0);
    }
    /* Bits the pinned source never names are reported, not silently dropped:
     * the handler still writes them back to GPU_INT_CLEAR. */
    {
        const NativeMaliIrq::Gpu gpu =
            NativeMaliIrq::decodeGpu(GPU_IRQ_FAULT | BIT(1) | BIT(31));
        CHECK(gpu.unknown == (BIT(1) | BIT(31)));
        CHECK(gpu.fault());
        CHECK(!gpu.multipleFault());
        CHECK(gpu.any());
    }
    {
        const NativeMaliIrq::Gpu gpu = NativeMaliIrq::decodeGpu(BIT(2) | BIT(11));
        CHECK(gpu.unknown == (BIT(2) | BIT(11)));
        CHECK(!gpu.error());
        CHECK(gpu.any());
    }
}

/* panfrost_job.c:537-551: DONE is BIT(js) and ERR is BIT(js + 16), and the same
 * word goes to JOB_INT_CLEAR. NUM_JOB_SLOTS is 3 (panfrost_job.h:48). */
void testJobDecode()
{
    CHECK(MK_JS_MASK(0) == 0x00010001u);
    CHECK(MK_JS_MASK(2) == 0x00040004u);
    CHECK(JOB_INT_MASK_DONE(2) == BIT(2));
    CHECK(JOB_INT_MASK_ERR(2) == BIT(18));

    {
        const NativeMaliIrq::Jobs jobs = NativeMaliIrq::decodeJobs(0, 3);
        CHECK(!jobs.any());
        CHECK(jobs.unknown == 0);
        CHECK(!jobs.doneOn(0) && !jobs.errorOn(0));
    }
    /* One slot's DONE, its ERR, and both together. */
    {
        const NativeMaliIrq::Jobs jobs =
            NativeMaliIrq::decodeJobs(JOB_INT_MASK_DONE(0), 3);
        CHECK(jobs.any());
        CHECK(jobs.done == BIT(0));
        CHECK(jobs.error == 0);
        CHECK(jobs.unknown == 0);
        CHECK(jobs.doneOn(0) && !jobs.errorOn(0));
        CHECK(jobs.status == JOB_INT_MASK_DONE(0));   /* clearable as-is */
    }
    {
        const NativeMaliIrq::Jobs jobs =
            NativeMaliIrq::decodeJobs(JOB_INT_MASK_ERR(2), 3);
        CHECK(jobs.error == BIT(2));
        CHECK(jobs.done == 0);
        CHECK(!jobs.doneOn(2) && jobs.errorOn(2));
    }
    {
        const NativeMaliIrq::Jobs jobs =
            NativeMaliIrq::decodeJobs(MK_JS_MASK(1), 3);
        CHECK(jobs.done == BIT(1) && jobs.error == BIT(1));
        CHECK(jobs.doneOn(1) && jobs.errorOn(1));
        CHECK(jobs.unknown == 0);
    }
    /* All three slots, the full ALL_JS_INT_MASK (panfrost_device.h:29-32). */
    {
        const uint32_t all = 0x00070007u;
        const NativeMaliIrq::Jobs jobs = NativeMaliIrq::decodeJobs(all, 3);
        CHECK(jobs.done == 0x7u && jobs.error == 0x7u && jobs.unknown == 0);
        for(uint32_t js = 0; js < 3; ++js)
            CHECK(jobs.doneOn(js) && jobs.errorOn(js));
    }
    /* Reserved bits: a slot the device does not have. The decode keeps them
     * apart instead of walking a slot number that GPU_JS_PRESENT denies. */
    {
        const NativeMaliIrq::Jobs jobs =
            NativeMaliIrq::decodeJobs(BIT(3) | BIT(19) | BIT(31), 3);
        CHECK(jobs.unknown == (BIT(3) | BIT(19) | BIT(31)));
        CHECK(!jobs.any());
        CHECK(jobs.status == (BIT(3) | BIT(19) | BIT(31)));
        CHECK(!jobs.doneOn(3) && !jobs.errorOn(3));
    }
    /* A full 16-slot palette still folds cleanly; 17 slots cannot be described
     * by two 16-bit halves, so nothing is claimed about the word. */
    {
        const NativeMaliIrq::Jobs jobs = NativeMaliIrq::decodeJobs(~0u, 16);
        CHECK(jobs.done == 0xFFFFu && jobs.error == 0xFFFFu);
        CHECK(jobs.unknown == 0);
        CHECK(jobs.doneOn(15) && jobs.errorOn(15));
    }
    {
        const NativeMaliIrq::Jobs none = NativeMaliIrq::decodeJobs(0x5, 0);
        CHECK(none.unknown == 0x5);
        CHECK(!none.any());
        const NativeMaliIrq::Jobs tooMany = NativeMaliIrq::decodeJobs(0x5, 17);
        CHECK(tooMany.unknown == 0x5);
    }
    /* Slot numbers outside 0..31 are answered, not shifted by 32. */
    {
        const NativeMaliIrq::Jobs jobs = NativeMaliIrq::decodeJobs(~0u, 3);
        CHECK(!jobs.doneOn(32) && !jobs.errorOn(32) && !jobs.doneOn(99));
    }
}

/* panfrost_mmu.c:888-890: as = ffs(status | (status >> 16)) - 1, and the clear
 * word is BIT(as) | BIT(as + 16). */
void testMmuDecode()
{
    CHECK(NativeMaliIrq::mmuClearMask(0) == 0x00010001u);
    CHECK(NativeMaliIrq::mmuClearMask(2) == 0x00040004u);
    CHECK(NativeMaliIrq::mmuClearMask(15) == 0x80008000u);
    CHECK(NativeMaliIrq::mmuClearMask(16) == 0);   /* only 16 AS exist */

    {
        const NativeMaliIrq::Mmu mmu = NativeMaliIrq::decodeMmu(0, 0x7);
        CHECK(!mmu.any());
        CHECK(mmu.fault == 0 && mmu.second == 0 && mmu.absent == 0);
    }
    /* The low half alone is the fault; the high half alone is the other source
     * the handler clears together with it. */
    {
        const NativeMaliIrq::Mmu mmu = NativeMaliIrq::decodeMmu(BIT(0), 0x7);
        CHECK(mmu.any());
        CHECK(mmu.faultOn(0) && !mmu.secondOn(0) && mmu.absent == 0);
        CHECK(mmu.status == BIT(0));
    }
    {
        const NativeMaliIrq::Mmu mmu = NativeMaliIrq::decodeMmu(BIT(16), 0x7);
        CHECK(mmu.secondOn(0) && !mmu.faultOn(0) && mmu.absent == 0);
    }
    {
        const NativeMaliIrq::Mmu mmu =
            NativeMaliIrq::decodeMmu(BIT(1) | BIT(17), 0x7);
        CHECK(mmu.faultOn(1) && mmu.secondOn(1));
        CHECK(mmu.fault == BIT(1) && mmu.second == BIT(1));
        CHECK(NativeMaliIrq::mmuClearMask(1) == (BIT(1) | BIT(17)));
    }
    /* An event on an address space GPU_AS_PRESENT denies is reported as such:
     * the source would walk it and then read a fault record that cannot be
     * there. */
    {
        const NativeMaliIrq::Mmu mmu = NativeMaliIrq::decodeMmu(BIT(1), 0x5);
        CHECK(mmu.absent == BIT(1));
        CHECK(mmu.fault == 0 && mmu.second == 0);
        CHECK(mmu.any());
        CHECK(!mmu.faultOn(1));
    }
    {
        const NativeMaliIrq::Mmu mmu =
            NativeMaliIrq::decodeMmu(BIT(2) | BIT(18) | BIT(3), 0x5);
        CHECK(mmu.fault == BIT(2) && mmu.second == BIT(2));
        CHECK(mmu.absent == BIT(3));
    }
    /* Including an absent AS that only shows up in the high half. */
    {
        const NativeMaliIrq::Mmu mmu =
            NativeMaliIrq::decodeMmu(BIT(17) | BIT(4), 0x5);
        CHECK(mmu.fault == 0 && mmu.second == 0);
        CHECK(mmu.absent == (BIT(1) | BIT(4)));
    }
    /* The two halves together are exactly the set the handler iterates. */
    {
        const uint32_t status = BIT(0) | BIT(5) | BIT(16) | BIT(20);
        const NativeMaliIrq::Mmu mmu = NativeMaliIrq::decodeMmu(status, 0xFF);
        CHECK(((mmu.fault | mmu.second) & 0xFFFF) ==
            ((status | (status >> 16)) & 0xFFFF));
        CHECK(mmu.absent == 0);
    }
    {
        const NativeMaliIrq::Mmu mmu = NativeMaliIrq::decodeMmu(~0u, 0xFFFF);
        CHECK(mmu.fault == 0xFFFFu && mmu.second == 0xFFFFu);
        CHECK(mmu.absent == 0);
        CHECK(!mmu.faultOn(32));
    }
}

} // namespace

int main()
{
    testGpuErrorPair();
    testGpuDecode();
    testJobDecode();
    testMmuDecode();
    std::puts("PASS: GPU/JOB/MMU interrupt status decoding against the pinned "
        "handlers: the FAULT|MULTIPLE_FAULT error pair, one accessor per "
        "GPU_IRQ_* source with unnamed bits kept as unknown, per-slot DONE/ERR "
        "folded onto BIT(js) for NUM_JOB_SLOTS slots with reserved slots kept "
        "apart, and the MMU half-pair per address space with the clear word");
    return 0;
}
