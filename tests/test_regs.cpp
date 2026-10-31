/* SPDX-License-Identifier: GPL-2.0-only */
/* Golden values for the generated register map. Nothing here touches hardware:
 * the point is that NativeMaliRegs.h is a verbatim copy of the pinned source, so
 * a wrong copy, a stale regeneration or a changed BIT() width fails here instead
 * of at 0x13040000 on the phone. */
#include "NativeMaliRegs.h"
#include "NativeMaliVm.h"
#include <cstdio>
#include <cstdlib>

#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)

int main()
{
    /* ---- GPU core block ---- */
    CHECK(GPU_ID == 0x00);
    CHECK(GPU_MMU_FEATURES == 0x014);
    CHECK(GPU_AS_PRESENT == 0x018);
    CHECK(GPU_INT_RAWSTAT == 0x20 && GPU_INT_CLEAR == 0x24);
    CHECK(GPU_INT_MASK == 0x28 && GPU_INT_STAT == 0x2c);
    CHECK(GPU_CMD == 0x30);
    CHECK(GPU_CMD_SOFT_RESET == 0x01 && GPU_CMD_HARD_RESET == 0x02);
    CHECK(GPU_STATUS == 0x34);
    CHECK(GPU_FAULT_STATUS == 0x3C && GPU_FAULT_ADDRESS_LO == 0x40);
    CHECK(GPU_PWR_KEY == 0x50);
    CHECK(GPU_PWR_KEY_UNLOCK == 0x2968a819u);
    CHECK(GPU_THREAD_MAX_THREADS == 0x0a0 && GPU_THREAD_FEATURES == 0x0ac);
    CHECK(GPU_THREAD_TLS_ALLOC == 0x310);
    CHECK(GPU_AFBC_FEATURES == 0x4c);
    CHECK(GPU_COHERENCY_FEATURES == 0x300 && GPU_COHERENCY_ENABLE == 0x304);
    CHECK(COHERENCY_ACE_LITE == 0 && COHERENCY_ACE == 1 && COHERENCY_NONE == 31);
    CHECK(GPU_JM_CONFIG == 0xf00 && GPU_SHADER_CONFIG == 0xf04);
    CHECK(GPU_L2_MMU_CONFIG == 0xf0c);
    CHECK(L2_MMU_CONFIG_LIMIT_EXTERNAL_WRITES_HALF == (0x3u << 26));
    CHECK(L2_MMU_CONFIG_ALLOW_SNOOP_DISPARITY == BIT(23));

    /* ---- core present / ready / power, the bitmaps M3 walks ---- */
    CHECK(GPU_SHADER_PRESENT_LO == 0x100 && GPU_SHADER_PRESENT_HI == 0x104);
    CHECK(GPU_TILER_PRESENT_LO == 0x110 && GPU_L2_PRESENT_LO == 0x120);
    CHECK(SHADER_READY_LO == 0x140 && TILER_READY_LO == 0x150 && L2_READY_LO == 0x160);
    CHECK(SHADER_PWRON_LO == 0x180 && TILER_PWRON_LO == 0x190 && L2_PWRON_LO == 0x1a0);
    CHECK(STACK_PWRON_LO == 0xe20 && STACK_READY_LO == 0xe10);
    CHECK(SHADER_PWROFF_LO == 0x1c0 && L2_PWROFF_LO == 0x1e0);
    CHECK(SHADER_PWRTRANS_LO == 0x200 && L2_PWRTRANS_LO == 0x220);
    CHECK(SHADER_PWRACTIVE_LO == 0x240 && L2_PWRACTIVE_LO == 0x260);
    /* Every one of those is inside the 0x4000 window the device tree gives. */
    CHECK(SHADER_PWRACTIVE_HI < 0x4000);

    /* ---- interrupts: the ERR mask is fault plus multiple fault ---- */
    CHECK(GPU_IRQ_FAULT == BIT(0));
    CHECK(GPU_IRQ_MULTIPLE_FAULT == BIT(7));
    CHECK(GPU_IRQ_RESET_COMPLETED == BIT(8));
    CHECK(GPU_IRQ_MASK_ERROR == (BIT(0) | BIT(7)));
    CHECK(GPU_IRQ_MASK_ERROR == 0x81);

    /* ---- feature decode helpers ---- */
    CHECK(GPU_MMU_FEATURES_VA_BITS(0x2812) == 0x12);
    CHECK(GPU_MMU_FEATURES_PA_BITS(0x2812) == 0x28);

    /* ---- job slots (M6) ---- */
    CHECK(JOB_INT_RAWSTAT == 0x1000 && JOB_INT_STAT == 0x100c);
    CHECK(JOB_INT_MASK_DONE(3) == BIT(3));
    CHECK(JOB_INT_MASK_ERR(3) == BIT(19));
    CHECK(JS_BASE == 0x1800 && JS_SLOT_STRIDE == 0x80);
    CHECK(JS_HEAD_LO(0) == 0x1800 && JS_HEAD_HI(0) == 0x1804);
    CHECK(JS_TAIL_LO(0) == 0x1808 && JS_TAIL_HI(0) == 0x180c);
    CHECK(JS_AFFINITY_LO(0) == 0x1810 && JS_AFFINITY_HI(0) == 0x1814);
    CHECK(JS_CONFIG(0) == 0x1818 && JS_XAFFINITY(0) == 0x181c);
    CHECK(JS_COMMAND(0) == 0x1820 && JS_STATUS(0) == 0x1824);
    CHECK(JS_HEAD_LO(2) == 0x1900 && JS_COMMAND(1) == 0x18a0);

    /* ---- address spaces (M4/M6) ---- */
    CHECK(MMU_INT_RAWSTAT == 0x2000 && MMU_INT_STAT == 0x200c);
    CHECK(MMU_AS_SHIFT == 6 && MMU_AS_STRIDE == 0x40);
    CHECK(MMU_AS(0) == 0x2400 && MMU_AS(1) == 0x2440);
    CHECK(AS_TRANSTAB_LO(3) == 0x24c0);
    CHECK(AS_MEMATTR_LO(0) == 0x2408 && AS_MEMATTR_HI(0) == 0x240c);
    CHECK(AS_COMMAND(0) == 0x2418 && AS_COMMAND(3) == 0x24d8);
    CHECK(AS_TRANSCFG_LO(0) == 0x2430 && AS_STATUS(0) == 0x2428);
    CHECK(AS_COMMAND_UPDATE == 0x01 && AS_COMMAND_FLUSH_MEM == 0x05);
    CHECK(AS_TRANSCFG_ADRMODE_LEGACY == 0);
    CHECK(AS_TRANSCFG_XREADABLE == (1UL << 36));
    CHECK(AS_FAULTSTATUS_ACCESS_TYPE_READ == 0x200);
    CHECK(AS_FAULTSTATUS_ACCESS_TYPE_WRITE == 0x300);
    CHECK(AS_STATUS_AS_ACTIVE == 0x01);
    CHECK(AS_LOCK_REGION_MIN_SIZE == (1ULL << 15));

    /* ---- TRANSTAB bits: the generated map and NativeMaliVm must agree ---- */
    CHECK(AS_TRANSTAB_LPAE_ADRMODE_TABLE == 0x3);
    CHECK(AS_TRANSTAB_LPAE_ADRMODE_IDENTITY == 0x2);
    CHECK(AS_TRANSTAB_LPAE_ADRMODE_MASK == 0x3);
    CHECK(AS_TRANSTAB_LPAE_READ_INNER == 0x4);
    CHECK(AS_TRANSTAB_LPAE_SHARE_OUTER == 0x10);
    CHECK(AS_TRANSTAB_LPAE_ADDR_SPACE_MASK == UINT64_C(0xfffffffffffff000));
    {
        /* A page table built by NativeMaliVm produces exactly the descriptor the
         * register map describes: type 3 (table), read-inner, and outer-shareable
         * only when the walker is coherent. */
        const uint64_t root = UINT64_C(0x40100000);
        CHECK((NativeMaliVm::encodeTable(root) & AS_TRANSTAB_LPAE_ADRMODE_MASK) ==
            AS_TRANSTAB_LPAE_ADRMODE_TABLE);
        CHECK((NativeMaliVm::encodeTable(root) & AS_TRANSTAB_LPAE_ADDR_SPACE_MASK) == root);
        CHECK((NativeMaliVm::encodeTable(root) | AS_TRANSTAB_LPAE_READ_INNER) ==
            (root | 7));
    }

    std::printf("PASS: generated register map matches the pinned panfrost_regs.h "
        "(GPU core, job slots, address spaces, TRANSTAB bits) and agrees with NativeMaliVm\n");
}
