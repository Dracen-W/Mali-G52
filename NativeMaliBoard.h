/* SPDX-License-Identifier: GPL-2.0-only */
/* Device-tree description of the MT6768 Mali node. Derived from
 * NativeAdrenoBoard (MIT, Dracen): same parsing rules and refusal style.
 *
 * Every fact below is from this phone's own device tree, which the tests read
 * from tests/data/mt6768.dtb:
 *
 *   /mali@13040000
 *     compatible = "mediatek,mali", "arm,mali-valhall"
 *     reg = <0x0 0x13040000 0x0 0x4000>          (2 address, 2 size cells)
 *     interrupts = <0 274 8 0 275 8 0 276 8 0 277 8 0 278 8>
 *     interrupt-names = "GPU", "MMU", "JOB", "EVENT", "PWR"
 *     (no iommus property: the GPU uses its own MMU, not the platform M4U)
 *
 *   /gpufreq
 *     clocks = <&topckgen 0x54 &topckgen 0x26 &topckgen 0x06 &mfg_cfg 0x00
 *               &scpsys 0x09 &scpsys 0x04 &scpsys 0x07 &scpsys 0x08>
 *     clock-names = "clk_mux", "clk_main_parent", "clk_sub_parent",
 *                   "subsys_mfg_cg", "mtcmos_mfg_async", "mtcmos_mfg",
 *                   "mtcmos_mfg_core0", "mtcmos_mfg_core1"
 *
 * Note the last four are power-domain clocks of scpsys, not topckgen clocks.
 *
 * This parses only: it maps nothing, powers nothing, resolves no phandle and
 * never touches MMIO. Reading 0x13040000 before the MFG domains are known to be
 * powered can hang the bus, so the caller must establish that first (M1/M2).
 */
#ifndef NATIVE_MALI_BOARD_H
#define NATIVE_MALI_BOARD_H
#include <IOKit/IOTypes.h>
#include <stddef.h>
#include <stdint.h>

struct NativeMaliWindow {
    uint64_t base;
    uint64_t bytes;
};
enum { NativeMaliMaxWindows = 8 };

/* The five interrupts this device names. The order of interrupt-names is a
 * property of the DT, not of panfrost (panfrost numbers job/mmu/gpu), so the
 * decoder is indexed by name. EVENT and PWR are vendor sources whose meaning is
 * unknown; they are parsed and reported, and stay masked until proven. */
enum NativeMaliIrqIndex {
    NativeMaliIrqGpu = 0,
    NativeMaliIrqMmu,
    NativeMaliIrqJob,
    NativeMaliIrqEvent,
    NativeMaliIrqPwr,
    NativeMaliIrqCount
};

/* One GIC specifier as stored: <type number flags>. This board's DT binds all
 * five sources with flags 0x8, which include/dt-bindings/interrupt-controller/
 * irq.h names IRQ_TYPE_LEVEL_LOW. The project owner confirmed on 2026-10-25
 * that the device tree wins over the roadmap's "level high", so the expected
 * trigger is a fact here and is checked before any source is wired: a level-low
 * line registered as edge-triggered loses interrupts, and the wrong polarity can
 * leave the line asserted after the handler returns. */
/* IRQ_TYPE_LEVEL_HIGH and IRQ_TYPE_LEVEL_LOW from that same header. */
enum : uint32_t { NativeMaliIrqLevelHigh = 4, NativeMaliIrqLevelLow = 8 };
struct NativeMaliInterrupt {
    uint32_t type;
    uint32_t number;
    uint32_t flags;
};
struct NativeMaliInterruptSet {
    NativeMaliInterrupt entry[NativeMaliIrqCount];
};

/* One clock specifier: the provider phandle and its first argument. Providers
 * publish #clock-cells = <1> in this DT (topckgen, mfgcfg, scpsys), so the
 * caller passes cells_per_entry = 1. Only the first argument is kept; a clock
 * with more specifier cells is refused rather than truncated. */
struct NativeMaliClock {
    uint32_t phandle;
    uint32_t id;
};
enum { NativeMaliMaxClocks = 8 };

class NativeMaliBoard final {
    NativeMaliBoard() = delete;
public:
    /* Parse a `reg` property: big-endian 32-bit cells as stored in the FDT, with
     * the node's own #address-cells/#size-cells. Only 1 or 2 cells per value are
     * accepted; zero-length and overflowing windows are refused; the output
     * count is the exact number of complete windows. Nothing is written on
     * failure. */
    static IOReturn parseReg(const uint8_t *cells,size_t length,uint32_t address_cells,
        uint32_t size_cells,NativeMaliWindow *windows,size_t capacity,size_t *count);

    /* Parse `interrupts` together with `interrupt-names`. The GIC specifier is
     * three cells <type number flags>; anything else is refused instead of
     * guessed. All five names must be present exactly once and may appear in any
     * order; numbering follows the names, not the property order. Nothing is
     * written on failure. */
    static IOReturn parseInterrupts(const uint8_t *cells,size_t length,
        uint32_t cells_per_entry,const uint8_t *names,size_t names_length,
        NativeMaliInterruptSet *set);

    /* Parse `clocks` together with `clock-names`: one phandle plus
     * cells_per_entry specifier cells per entry, named in property order. Names
     * must be unique and the counts must agree. Nothing is written on failure. */
    static IOReturn parseClocks(const uint8_t *cells,size_t length,
        uint32_t cells_per_entry,const uint8_t *names,size_t names_length,
        NativeMaliClock *clocks,size_t capacity,size_t *count);

    /* Whether every parsed source is a GIC SPI asserted level-low, which is what
     * this board wires (see above). Refused rather than reinterpreted, so a DT
     * change is noticed at probe time instead of as a lost interrupt. */
    static IOReturn checkTriggers(const NativeMaliInterruptSet *set);

    /* The index-th NUL-terminated string of a DT string list, borrowed from the
     * caller's buffer. Refuses a truncated list, an empty string or a bad index. */
    static IOReturn stringAt(const uint8_t *names,size_t length,size_t index,
        const char **text,size_t *bytes);

    /* Number of strings in a DT string list; 0 for a malformed list. */
    static size_t stringCount(const uint8_t *names,size_t length);
};
#endif
