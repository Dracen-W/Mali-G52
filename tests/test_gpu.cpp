/* SPDX-License-Identifier: GPL-2.0-only */
/* NativeMaliGpu over synthetic GPU_ID values. This qualifies the decode, not the
 * chip: the value this phone actually reports is only known once M3 can read the
 * register, and an unknown id is refused rather than guessed. */
#include "NativeMaliGpu.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)

static NativeMaliGpuIdentity idOf(uint16_t product,uint16_t revision)
{
    return {product, revision, 0, 0, 0, 0, 0, 0, nullptr};
}

int main()
{
    NativeMaliGpuIdentity id{};

    /* ---- the value this phone actually reports ----
     * other reference/grab_out/mali_dmesg.txt, from the vendor kernel:
     *   Register LUT 00070200 ... arch 7.4.0
     *   GPU identified as 0x2 arch 7.4.0 r1p0 status 0
     * Product 0x7002 (g52) and revision 0x1000 (major 1, minor 0, status 0). */
    CHECK(NativeMaliGpu::identify(0x70021000u,&id) == kIOReturnSuccess);
    CHECK(id.product == 0x7002 && id.matched == 0x7002);
    CHECK(id.revision == 0x1000 && id.major == 1 && id.minor == 0 && id.status == 0);
    CHECK(id.bifrost == 1 && std::strcmp(id.model,"g52") == 0);
    CHECK(id.revision_specific == 0);

    /* G52 r0p0: GPU_ID = product << 16 | revision. */
    CHECK(NativeMaliGpu::identify(0x70020000u,&id) == kIOReturnSuccess);
    CHECK(id.product == 0x7002 && id.matched == 0x7002);
    CHECK(id.revision == 0 && id.major == 0 && id.minor == 0 && id.status == 0);
    CHECK(id.bifrost == 1);
    CHECK(std::strcmp(id.model,"g52") == 0);
    CHECK(id.revision_specific == 0);   /* the source has no per-rev entry */

    /* Revision decode: major 1, minor 0x23, status 4. G52 still matches. */
    CHECK(NativeMaliGpu::identify(0x70021234u,&id) == kIOReturnSuccess);
    CHECK(id.revision == 0x1234);
    CHECK(id.major == 1 && id.minor == 0x23 && id.status == 4);
    CHECK(std::strcmp(id.model,"g52") == 0);

    /* Bifrost models the source lists, each with no revision restriction. */
    static const struct {uint16_t product,revision;const char *model;} known[] = {
        {0x6000,0x0001,"g71"}, {0x6001,0x0000,"g72"}, {0x7000,0x0000,"g51"},
        {0x7001,0x0000,"g76"}, {0x7003,0x1000,"g31"}, {0x9001,0x0000,"g57"},
    };
    for(const auto &entry:known) {
        CHECK(NativeMaliGpu::identify(uint32_t(entry.product) << 16 | entry.revision,&id)
            == kIOReturnSuccess);
        CHECK(std::strcmp(id.model,entry.model) == 0);
        CHECK(id.bifrost == 1);
    }
    /* A revision with no entry is still the same model: panfrost only uses such
     * an entry to add issue bits, it does not reject the probe. */
    CHECK(NativeMaliGpu::identify(0x70030000u,&id) == kIOReturnSuccess);
    CHECK(std::strcmp(id.model,"g31") == 0 && id.revision_specific == 0);
    CHECK(NativeMaliGpu::identify(0x70031000u,&id) == kIOReturnSuccess);
    CHECK(id.revision_specific == 1);
    CHECK(NativeMaliGpu::identify(0x90010000u,&id) == kIOReturnSuccess);
    CHECK(id.revision_specific == 1);

    /* The product comparison masks bit 12..15 when they are set: 0x7102 and
     * 0x7002 are the same model to panfrost, 0x7003 is not. */
    CHECK(NativeMaliGpu::maskProduct(0x7002) == 0x7002);
    CHECK(NativeMaliGpu::maskProduct(0x7102) == 0x7002);
    CHECK(NativeMaliGpu::maskProduct(0x0600) == 0x0600);   /* Midgard: not masked */
    CHECK(NativeMaliGpu::maskProduct(0x0750) == 0x0750);
    CHECK(NativeMaliGpu::identify(0x71020000u,&id) == kIOReturnSuccess);
    CHECK(id.matched == 0x7002);

    /* The T60x fixup happens before matching: 0x6956 becomes Midgard 0x0600,
     * which this Bifrost-only driver refuses, with the fixed id reported. */
    CHECK(NativeMaliGpu::identify(0x69560010u,&id) == kIOReturnUnsupported);
    CHECK(id.product == 0x0600 && id.revision == 0x0010);
    CHECK(id.bifrost == 0 && id.model == nullptr);
    CHECK(id.major == 0 && id.minor == 1 && id.status == 0);

    /* Midgard and unknown Bifrost ids: refused, but decoded, so the caller can
     * log what the hardware said. No feature bits are guessed. */
    CHECK(NativeMaliGpu::identify(0x07500000u,&id) == kIOReturnUnsupported);
    CHECK(id.bifrost == 0 && id.model == nullptr);
    CHECK(NativeMaliGpu::identify(0x99990000u,&id) == kIOReturnUnsupported);
    CHECK(id.product == 0x9999 && id.bifrost == 1 && id.model == nullptr);
    CHECK(id.matched == 0);
    CHECK(NativeMaliGpu::identify(0x00000000u,&id) == kIOReturnUnsupported);
    CHECK(id.bifrost == 0 && id.model == nullptr);

    /* A null output is a programming error, not an unknown chip. */
    CHECK(NativeMaliGpu::identify(0x70020000u,nullptr) == kIOReturnBadArgument);

    /* A refused call still filled the caller's struct, so a stale model name
     * from an earlier call cannot survive. */
    id = idOf(0x7002,0);
    id.model = "stale";
    id.matched = 0xdead;
    CHECK(NativeMaliGpu::identify(0x99990000u,&id) == kIOReturnUnsupported);
    CHECK(id.model == nullptr && id.matched == 0 && id.revision_specific == 0);

    std::printf("PASS: GPU identity decode (product/revision split, T60x fixup, product "
        "masking, the Bifrost model table, revision entries, refusals)\n");
}
