/* SPDX-License-Identifier: GPL-2.0-only */
/* M2: GPU voltage, clock source, clock gate and the four MFG power domains.
 *
 * Read this before changing a number. Everything below is from the vendor
 * kernel that shipped with this SoC family (third_party/mtk-android/, not
 * committed; docs/MTK_SOURCES.md records the provenance):
 *
 * 1. Block bases
 *      arch/arm64/boot/dts/mediatek/mt6768.dts
 *        :568-572 infracfg_ao@10001000
 *        :575-583 scpsys@10001000, whose reg list names SPM at 0x10006000
 *                   (this is what closes the old "SPM base unconfirmed" note)
 *        :661-663 topckgen@10000000
 *        :812-813 apmixed@1000c000        (not used yet: no PLL programming)
 *        :2831-2833 mfg_cfg@13000000
 *      The caller maps each page; this class only sees offsets inside them.
 *
 * 2. SPM (base 0x10006000)
 *      drivers/clk/mediatek/clk-mt6768-pg.c
 *        :188 MFG_CORE1_PWR_CON  = 0x0310
 *        :196 MFG_ASYNC_PWR_CON  = 0x0334
 *        :197 MFG_PWR_CON        = 0x0338
 *        :202 MFG_CORE0_PWR_CON  = 0x034c
 *        :184 PWR_STATUS         = 0x0180
 *        :185 PWR_STATUS_2ND     = 0x0184
 *        :366-369 MFG/CORE0/CORE1/ASYNC status bits = 1<<11 / 12 / 13 / 14
 *      The SPM header in the archive names a MFG status bit at bit 4; that is
 *      stale. The clock/power driver that actually owns these domains uses
 *      bit 11, and its power-on loops check it. Bit 11 is what this file uses.
 *
 * 3. PWR_CON bits (identical in all four registers)
 *      clk-mt6768-pg.c:166-172
 *        PWR_RST_B=1<<0, PWR_ISO=1<<1, PWR_ON=1<<2, PWR_ON_2ND=1<<3,
 *        PWR_CLK_DIS=1<<4
 *      plus SRAM_PDN=1<<8 and SRAM_PDN_ACK=1<<12
 *      (clk-mt6768-pg.c:413-421)
 *
 * 4. Power-on sequence, copied step for step from the vendor functions:
 *      async: clk-mt6768-pg.c:2093-2160 (spm_mtcmos_ctrl_mfg_async_pwr)
 *      mfg:   clk-mt6768-pg.c:1525-1590 (bus protect, STA_POWER_ON branch)
 *             clk-mt6768-pg.c:1596-1656 (power, STA_POWER_ON branch)
 *      core0: clk-mt6768-pg.c:1893-1988
 *      core1: clk-mt6768-pg.c:1993-2088
 *      order: drivers/clk/mediatek/clk-mt6768-pg.c:3477-3485 puts async first
 *             and makes mfg its child, core0/core1 children of mfg; the
 *             gpufreq driver enables them in exactly that order
 *             (base/power/mt6768/mtk_gpufreq_core.c:439-459).
 *      The MFG bus protection is released *after* the domain is powered and
 *      its clock/reset bits are un-gated: pg_prepare enables power at
 *      clk-mt6768-pg.c:3242 and calls the bus-protect prepare hook afterwards
 *      (clk-mt6768-pg.c:3268-3269, which reaches
 *      spm_mtcmos_ctrl_mfg_bus_prot(STA_POWER_ON) at :1525-1590).
 *      Power-down is the exact reverse: pg_unprepare calls the bus-protect
 *      hook first (:3295) and powers the domain down afterwards (:3319).
 *
 * 5. Clock source
 *      clk-mt6768.c:225-231  CLK_CFG_UPDATE=0x004, CLK_CFG_1=0x50,
 *                            CLK_CFG_1_SET=0x54, CLK_CFG_1_CLR=0x58
 *      clk-mt6768.c:1002-1003  mfg_sel: shift 0, width 2, mux gate bit 7,
 *                              update bit 4
 *      clk-mt6768.c:670-675  mfg parents: 0=clk26m, 1=mfgpll_ck,
 *                            2=syspll_d3, 3=univpll_d3
 *      clk-mt6768.c:205-228 (clk-mux.c) set_parent writes the mask to CLR,
 *      the value to SET, then the update bit. clk_prepare_enable clears the
 *      mux gate: write bit 7 to CLR. This driver does the same and leaves the
 *      mux on clk26m (26 MHz), which is safe without programming MFGPLL.
 *
 * 6. MFG clock gate
 *      clk-mt6768.c:2020-2023 mfgcfg registers: status 0x0, set 0x4, clear 0x8
 *      clk-mt6768.c:2044 BG3D gate, shift 0
 *      Release = write 1 to 0x8. Vendor does this last, after MTCMOS
 *      (base/power/mt6785/mtk_gpufreq_core.c:429-547: buck, then mtcmos,
 *      then CG; the MT6768 archive lacks its own platform file).
 *
 * 7. Voltage: NativeMaliMt6358.h, fixed at the lowest vendor OPP.
 *
 * Writes are off by default. Build with -DNATIVE_MALI_M2_WRITES=1 only after a
 * human has looked at probe() on the device and accepted the read values
 * (roadmap rule 2). Host tests do exactly that; the driver does not.
 */
#ifndef NATIVE_MALI_POWER_H
#define NATIVE_MALI_POWER_H
#include "NativeMaliMt6358.h"
#include "NativeMaliPlatform.h"
#include <IOKit/IOTypes.h>
#include <stdint.h>

#ifndef NATIVE_MALI_M2_WRITES
#define NATIVE_MALI_M2_WRITES 0
#endif

struct NativeMaliPowerBlocks {
    NativeMaliRegisterTarget *spm = nullptr;       /* 0x10006000 */
    NativeMaliRegisterTarget *infracfg = nullptr;  /* 0x10001000 */
    NativeMaliRegisterTarget *topckgen = nullptr;  /* 0x10000000 */
    NativeMaliRegisterTarget *mfgcfg = nullptr;    /* 0x13000000 */
    NativeMaliRegisterTarget *pwrap = nullptr;     /* 0x1000D000 */
};

/* What the read-only probe saw. Every field is a raw word; no interpretation,
 * so a wrong offset shows up as a wrong-looking number instead of as a silent
 * "domain on" claim. */
struct NativeMaliPowerObservation {
    uint32_t spmStatus = 0;
    uint32_t spmStatus2 = 0;
    uint32_t infraProtectEnable = 0;
    uint32_t infraProtectStatus1 = 0;
    uint32_t topMfgClockConfig = 0;
    uint32_t topClockUpdate = 0;
    uint32_t mfgClockGateStatus = 0;
    NativeMaliMt6358::RailState vgpu;
    NativeMaliMt6358::RailState vsram;
};

class NativeMaliPower final {
public:
    enum : uint32_t {
        SpmPowerStatus = 0x180,
        SpmPowerStatus2 = 0x184,
        SpmMfgAsyncPowerCon = 0x334,
        SpmMfgPowerCon = 0x338,
        SpmMfgCore0PowerCon = 0x34C,
        SpmMfgCore1PowerCon = 0x310,

        SpmPowerStatusMfg = 1u << 11,
        SpmPowerStatusMfgCore0 = 1u << 12,
        SpmPowerStatusMfgCore1 = 1u << 13,
        SpmPowerStatusMfgAsync = 1u << 14,

        PowerConResetB = 1u << 0,
        PowerConIso = 1u << 1,
        PowerConOn = 1u << 2,
        PowerConOnSecond = 1u << 3,
        PowerConClockDisable = 1u << 4,
        PowerConSramPowerDown = 1u << 8,
        PowerConSramPowerDownAck = 1u << 12
    };
    enum : uint32_t {
        InfraProtectEnable = 0x220,
        InfraProtectStatus0 = 0x224,
        InfraProtectStatus1 = 0x228,
        InfraProtectSet = 0x2A0,
        InfraProtectClear = 0x2A4,
        MfgProtectStep1 = 1u << 25,
        MfgProtectStep2 = (1u << 21) | (1u << 22)
    };
    enum : uint32_t {
        TopClockUpdate = 0x004,
        TopMfgClockConfig = 0x050,
        TopMfgClockConfigSet = 0x054,
        TopMfgClockConfigClear = 0x058,
        TopMfgSelectShift = 0,
        TopMfgSelectMask = 0x3,
        TopMfgSelectClk26m = 0,
        TopMfgMuxGate = 1u << 7,
        TopMfgUpdate = 1u << 4
    };
    enum : uint32_t {
        MfgClockGateStatus = 0x0,
        MfgClockGateSet = 0x4,
        MfgClockGateClear = 0x8,
        MfgClockGateBg3d = 1u << 0
    };
    enum : uint64_t { DefaultTimeoutUs = 2000, DefaultPollUs = 10 };

    /* Why gpuReadable() said no. */
    enum : uint32_t {
        GpuReadMissingDomains = 1u << 0,
        GpuReadMissingClockMux = 1u << 1,
        GpuReadMissingClockGate = 1u << 2
    };

    /* False unless the build explicitly opts in; see the header comment. */
    static constexpr bool kWriteAuthorized = NATIVE_MALI_M2_WRITES != 0;

    NativeMaliPower() = default;
    ~NativeMaliPower() = default;
    NativeMaliPower(const NativeMaliPower &) = delete;
    NativeMaliPower &operator=(const NativeMaliPower &) = delete;

    /* Requires every block: the probe must be able to see all of them, and a
     * later step must not discover a missing mapping halfway through. */
    IOReturn prepare(const NativeMaliPowerBlocks &blocks);
    bool prepared() const;

    /* Read-only in the sense that no configuration register is changed; pwrap
     * reads still acknowledge the wrapper's valid bit, which is why this is not
     * a const member. Safe with the GPU off: SPM and the clock blocks are always
     * powered, and the PMIC rails are readable through pwrap. */
    IOReturn probe(NativeMaliPowerObservation *observation);

    /* Read-only safety check for the GPU window (0x13040000). The block is only
     * safe to read once the four MFG domains report on in both PWR_STATUS
     * registers, the topckgen mfg mux gate is open and the mfgcfg BG3D gate is
     * open. Returns kIOReturnNotReady with the missing parts ORed into
     * `missing`; a failed register read is returned unchanged. The mux parent
     * is not checked: a bootloader may have left the GPU on a parent other than
     * clk26m, and any enabled parent is enough to read registers. This is a
     * bring-up gate, not a lock; it does not protect against a concurrent
     * power-down. */
    IOReturn gpuReadable(uint32_t *missing = nullptr) const;

    /* Writes, all refused unless kWriteAuthorized. */
    IOReturn setFixedVoltage();
    /* Top mfg_sel -> clk26m, mux gate released, update bit pulsed. */
    IOReturn selectMfgClockSource();
    /* Release subsys_mfg_cg. Call after the domains are up. */
    IOReturn enableMfgClockGate();
    /* async, mfg, core0, core1. Each domain goes through the vendor's exact
     * steps (NativeMaliPower.h section 4). If one fails, the domains already
     * up are powered down in reverse order and the first error is returned. */
    IOReturn powerOnDomains(uint64_t timeout_us = DefaultTimeoutUs);

    /* voltage -> clock source -> domains -> clock gate. On failure after the
     * domains came up they are powered down again; the rails are left at the
     * fixed conservative voltage (they are already at it, and dropping them
     * gains nothing), and the error is returned unchanged. */
    IOReturn bringUp(uint64_t timeout_us = DefaultTimeoutUs);

    /* Close subsys_mfg_cg: write 1 to the gate's SET register and read the
     * status bit back. This is the vendor's clk_disable_unprepare(subsys_mfg_cg)
     * (mtk_gpufreq_core.c:415-432, the call at :425); clk-gate.c:167-175 is why
     * the status bit reads 1 once the gate is closed. Refused unless
     * kWriteAuthorized. */
    IOReturn disableMfgClockGate();

    /* core1, core0, mfg, async: the vendor's mt_gpufreq_disable_MTCMOS()
     * (mtk_gpufreq_core.c:466-496, the four calls at :478-481), which is the
     * reverse of the order powerOnDomains() uses and of the clock parent
     * hierarchy (clk-mt6768-pg.c:3477-3485).
     *
     * Every domain is attempted even when an earlier one fails, so a wedged
     * transition does not leave the cheaper domains powered; the *first* error
     * is returned unchanged (the same shape as panfrost_gpu_power_off(),
     * panfrost_gpu.c:486-504). A caller that gets an error must not report the
     * block as powered down: re-run this, the steps are idempotent for a domain
     * that already reads off. */
    IOReturn powerDownDomains(uint64_t timeout_us = DefaultTimeoutUs);

    /* The whole M2 sequence backwards: clock gate, then the four domains. Both
     * halves are attempted and the first error is returned.
     *
     * Deliberately not mirrored from the vendor:
     *   - the two bucks. __mt_gpufreq_bucks_disable() (mtk_gpufreq_core.c:
     *     2036-2068) turns VGPU then VSRAM_GPU off, but this driver never
     *     turned them on: setFixedVoltage() only writes VOSEL and both rails
     *     are already enabled by the bootloader (NativeMaliMt6358::setFixed()).
     *     Disabling a rail this driver did not enable, with no enable path to
     *     put it back, would leave the next bringUp starting from a state no
     *     source describes.
     *   - the topckgen mfg mux gate. The vendor only opens it around a parent
     *     change (mtk_gpufreq_core.c:1871-1900), and gpuReadable() treats an
     *     open gate as "the GPU window may be read"; closing it here would
     *     change the meaning of the M1 probe on a device that is already up.
     */
    IOReturn tearDown(uint64_t timeout_us = DefaultTimeoutUs);

    /* The same poll the domain steps use, without touching any register. */
    static IOReturn waitFor(NativeMaliRegisterTarget *target, uint32_t offset,
        uint32_t mask, uint64_t timeout_us, uint32_t poll_us, uint32_t *last);

private:
    struct Domain {
        uint32_t control;
        uint32_t status;
        bool bus_protect;
        bool sram_power_down;
    };

    /* async, mfg, core0, core1, with the properties each step needs. One table
     * so the power-on order and its reverse cannot drift apart. */
    static const Domain *mfgDomains();

    IOReturn readWord(NativeMaliRegisterTarget *target, uint32_t offset,
        uint32_t *value) const;
    IOReturn writeWord(NativeMaliRegisterTarget *target, uint32_t offset,
        uint32_t value) const;
    IOReturn modify(NativeMaliRegisterTarget *target, uint32_t offset,
        uint32_t clear, uint32_t set) const;
    IOReturn waitStatus(uint32_t mask, uint64_t timeout_us) const;
    IOReturn waitStatusOff(uint32_t mask, uint64_t timeout_us) const;
    IOReturn waitControl(uint32_t offset, uint32_t mask, bool set,
        uint64_t timeout_us) const;
    IOReturn releaseMfgBusProtect(uint64_t timeout_us) const;
    IOReturn assertMfgBusProtect(uint64_t timeout_us) const;
    IOReturn powerOn(const Domain &domain, uint64_t timeout_us) const;
    IOReturn powerOff(const Domain &domain, uint64_t timeout_us) const;

    NativeMaliRegisterTarget *spm_ = nullptr;
    NativeMaliRegisterTarget *infracfg_ = nullptr;
    NativeMaliRegisterTarget *topckgen_ = nullptr;
    NativeMaliRegisterTarget *mfgcfg_ = nullptr;
    NativeMaliPwrap pwrap_;
    NativeMaliMt6358 pmic_;
};
#endif
