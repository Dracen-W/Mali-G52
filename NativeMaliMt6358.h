/* SPDX-License-Identifier: GPL-2.0-only */
/* MT6358 regulators for the two GPU rails.
 *
 * Only two registers matter to the GPU, and their whole behaviour is a linear
 * range. Sources (third_party/mtk-android/, see docs/MTK_SOURCES.md):
 *
 *   voltage range, encoding
 *     drivers/regulator/mt6358-regulator.c:855-865
 *       MT_BUCK("buck_vgpu", VGPU, 500000, 1293750, 6250, 0, mt_volt_range1, ...)
 *     drivers/regulator/mt6358-regulator.c:975-983
 *       MT_LDO_REGULAR("ldo_vsram_gpu", VSRAM_GPU, 500000, 1293750, 6250,
 *                      0, mt_volt_range1, ...)
 *     drivers/regulator/mt6358-regulator.c:253-255
 *       mt_volt_range1 = REGULATOR_LINEAR_RANGE(500000, 0, 0x7f, 6250)
 *     therefore  uV = 500000 + selector * 6250,  selector in 0..0x7f.
 *
 *   PMIC register numbers (PMIC_BASE is 0, upmu_hw.h:12)
 *     drivers/misc/mediatek/include/mt-plat/mt6768/include/mach/upmu_hw.h
 *       :925  MT6358_BUCK_VGPU_CON0      = 0x1508   (enable, bit 0)
 *       :940  MT6358_BUCK_VGPU_ELR0      = 0x1526   (VOSEL, bits 6:0)
 *       :1378 MT6358_LDO_VSRAM_GPU_CON0  = 0x1bc8   (enable, bit 0)
 *       :1400 MT6358_LDO_VSRAM_CON3      = 0x1bf4   (VOSEL, bits 6:0)
 *       :9980-9981  PMIC_RG_BUCK_VGPU_EN_MASK/SHIFT      = 0x1 / 0
 *       :10111-10113 PMIC_RG_BUCK_VGPU_VOSEL_MASK/SHIFT  = 0x7f / 0
 *       :13977-13979 PMIC_RG_LDO_VSRAM_GPU_EN_MASK/SHIFT = 0x1 / 0
 *       :14115-14117 PMIC_RG_LDO_VSRAM_GPU_VOSEL_MASK/SHIFT = 0x7f / 0
 *
 *   the fixed voltage
 *     drivers/misc/mediatek/base/power/mt6768/mtk_gpufreq_core.h
 *       :76  SEG_GPU_DVFS_VOLT31  = 61250  (mV x 100 -> 612500 uV)
 *       :89  SEG_GPU_DVFS_VSRAM11 = 85000  (mV x 100 -> 850000 uV)
 *     Those are the lowest entries of the vendor's own OPP table. The phone's
 *     DT carries no GPU OPP (docs/DEVICE.md section 4), so the driver does not
 *     do DVFS: it holds these two values and never changes them.
 *
 *   order: VSRAM_GPU before VGPU
 *     drivers/misc/mediatek/base/power/mt6785/mtk_gpufreq_core.c:429-462
 *       (the MT6768 archive does not contain its own gpufreq platform file;
 *        docs/MTK_SOURCES.md records that gap. The enable order is the vendor
 *        one from the sibling file: ldo_vsram_gpu, then buck_vgpu.)
 *
 * Read-back: after every write the same register is read again and compared
 * with what was written. A mismatch is kIOReturnIOError and the previous word
 * is put back, because a regulator that silently did not take a value must not
 * be reported as set. The vendor uses the DA (debug) mirrors for read-back;
 * reading the RG register back is stricter and needs no second address, so
 * that is what this file does.
 */
#ifndef NATIVE_MALI_MT6358_H
#define NATIVE_MALI_MT6358_H
#include "NativeMaliPwrap.h"
#include <IOKit/IOTypes.h>
#include <stdint.h>

class NativeMaliMt6358 final {
public:
    enum : uint16_t {
        VgpuEnable = 0x1508,
        VgpuVosel = 0x1526,
        VsramEnable = 0x1BC8,
        VsramVosel = 0x1BF4,
        EnableMask = 0x1u << 0,
        VoselMask = 0x7F
    };
    enum : uint32_t {
        MinimumMicrovolts = 500000,
        MaximumMicrovolts = 1293750,
        StepMicrovolts = 6250,
        /* Lowest OPP of the vendor table, see the header comment. */
        FixedVgpuMicrovolts = 612500,
        FixedVsramMicrovolts = 850000
    };

    struct RailState {
        uint16_t enableWord = 0;   /* raw CON0, so a restore is exact */
        uint16_t voselWord = 0;    /* raw VOSEL register, upper bits kept */
        uint32_t microvolts = 0;
        bool enabled = false;
    };

    NativeMaliMt6358() = default;
    ~NativeMaliMt6358() = default;
    NativeMaliMt6358(const NativeMaliMt6358 &) = delete;
    NativeMaliMt6358 &operator=(const NativeMaliMt6358 &) = delete;

    IOReturn prepare(NativeMaliPwrap *pwrap);
    bool prepared() const { return pwrap_ != nullptr; }

    /* Exact multiples of 6250 inside 500000..1293750 only. A caller that asks
     * for 612501 gets kIOReturnBadArgument, not a rounded value. */
    static bool encode(uint32_t microvolts, uint16_t *selector);
    static bool decode(uint16_t selector, uint32_t *microvolts);

    IOReturn readVgpu(RailState *state);
    IOReturn readVsram(RailState *state);

    /* Sets the selector if it differs, then enables the rail if it is off.
     * Verifies both writes. `state` is filled with the values found before the
     * call so a caller can undo it. */
    IOReturn setVgpu(uint32_t microvolts, RailState *before = nullptr);
    IOReturn setVsram(uint32_t microvolts, RailState *before = nullptr);

    /* FixedVgpuMicrovolts / FixedVsramMicrovolts, VSRAM first. If the VGPU step
     * fails the VSRAM rail is put back to its previous words. */
    IOReturn setFixed();

private:
    IOReturn readRail(uint16_t enable_offset, uint16_t vosel_offset,
        RailState *state);
    IOReturn setRail(uint16_t enable_offset, uint16_t vosel_offset,
        uint32_t microvolts, RailState *before);
    IOReturn restoreRail(uint16_t enable_offset, uint16_t vosel_offset,
        const RailState &state);

    NativeMaliPwrap *pwrap_ = nullptr;
};
#endif
