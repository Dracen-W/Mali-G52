/* SPDX-License-Identifier: GPL-2.0-only */
/* PMIC wrapper (WACS2) transport for MT6768 + MT6358.
 *
 * This is the one path to the PMIC. It is deliberately just the protocol: the
 * register numbers of the regulators live in NativeMaliMt6358.h, the policy of
 * which voltage to use lives in NativeMaliPower.cpp.
 *
 * Every value below cites the vendor source that ships with this SoC family
 * (third_party/mtk-android/, see docs/MTK_SOURCES.md):
 *
 *   register offsets
 *     drivers/misc/mediatek/pmic_wrap/mt6768/pwrap_hal.h:569-571
 *       PMIC_WRAP_WACS2_CMD   = PMIC_WRAP_BASE + 0xC20
 *       PMIC_WRAP_WACS2_RDATA = PMIC_WRAP_BASE + 0xC24
 *       PMIC_WRAP_WACS2_VLDCLR= PMIC_WRAP_BASE + 0xC28
 *     PMIC_WRAP_BASE is 0x1000D000: the device's /proc/iomem dump and the
 *     board's pmic node both put pmic_wrap at that page (docs/DEVICE.md).
 *     The caller maps it; this class only knows offsets inside that page.
 *
 *   command word
 *     pwrap_hal.c:469-482 (pwrap_wacs2_hal)
 *       wacs_write = write << 31
 *       wacs_adr   = (adr >> 1) << 16          (PMIC register numbers are even)
 *       wacs_cmd   = wacs_write | wacs_adr | wdata
 *
 *   FSM
 *     pwrap_hal.h:219-223, 1161-1165
 *       WACS_FSM_IDLE = 0, WACS_FSM_WFVLDCLR = 6 (read data is valid)
 *       GET_WACS2_FSM(x)        = (x >> 16) & 7
 *       GET_WACS2_INIT_DONE2(x) = (x >> 21) & 1
 *     pwrap_hal.c:447-472 (pwrap_wacs2_hal): wait for idle before a command;
 *     on a read, wait for WFVLDCLR, take the low 16 bits, then write 1 to
 *     VLDCLR. A pending WFVLDCLR seen while waiting for idle is cleared the
 *     same way (pwrap_hal.c:390-410).
 *
 *   timeouts
 *     pwrap_hal.h:229-230  TIMEOUT_READ = TIMEOUT_WAIT_IDLE = 0x2710 us
 *
 * Two deliberate differences from the vendor code, both stricter, both marked
 * here so a later reader does not mistake them for a transcription slip:
 *   1. A write waits for the FSM to return to idle after the command. The
 *      vendor returns immediately; without the wait a failed write is
 *      indistinguishable from a successful one, and a regulator write is not
 *      something to report as done on faith.
 *   2. A read waits for idle again after VLDCLR. The next transaction would
 *      do it anyway; doing it here makes each read a complete transaction.
 */
#ifndef NATIVE_MALI_PWRAP_H
#define NATIVE_MALI_PWRAP_H
#include "NativeMaliPlatform.h"
#include <IOKit/IOTypes.h>
#include <stdint.h>

class NativeMaliPwrap final {
public:
    enum : uint32_t {
        Wacs2Command = 0xC20,
        Wacs2Data = 0xC24,
        Wacs2Clear = 0xC28,
        FsmShift = 16,
        FsmMask = 0x7,
        FsmIdle = 0,
        FsmWaitValidClear = 6,
        InitDone = 1u << 21,
        CommandWrite = 1u << 31,
        AddressShift = 16,
        DataMask = 0xFFFF
    };
    enum : uint64_t { DefaultTimeoutUs = 10000 };
    enum : uint32_t { DefaultPollUs = 10 };

    NativeMaliPwrap() = default;
    ~NativeMaliPwrap() = default;
    NativeMaliPwrap(const NativeMaliPwrap &) = delete;
    NativeMaliPwrap &operator=(const NativeMaliPwrap &) = delete;

    /* `target` must be the 0x1000D000 page. No register is touched here. */
    IOReturn prepare(NativeMaliRegisterTarget *target);
    bool prepared() const { return target_ != nullptr; }

    /* 16-bit PMIC register access. Both refuse kIOReturnNotReady until the
     * wrapper has reported INIT_DONE; neither invents a value on failure. */
    IOReturn read(uint16_t address, uint16_t *value,
        uint64_t timeout_us = DefaultTimeoutUs);
    IOReturn write(uint16_t address, uint16_t value,
        uint64_t timeout_us = DefaultTimeoutUs);

private:
    IOReturn waitIdle(uint64_t timeout_us);
    /* Poll RDATA until (word >> 16) & 7 == wanted. When `clear_valid` is set,
     * an FSM that is already WFVLDCLR is acknowledged and polling continues. */
    IOReturn waitForFsm(uint32_t wanted, bool clear_valid, uint32_t *word,
        uint64_t timeout_us);

    NativeMaliRegisterTarget *target_ = nullptr;
};
#endif
