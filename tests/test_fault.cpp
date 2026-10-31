/* SPDX-License-Identifier: GPL-2.0-only */
/* NativeMaliFault: the exception/access decode, with no registers involved.
 *
 * The golden names below are copied from the pinned panfrost sources:
 * DRM_PANFROST_EXCEPTION_* in panfrost_device.h:258-330 and the
 * PANFROST_EXCEPTION() table in panfrost_device.c:425-500. The table is sparse
 * and 0xF0 entries long, so every hole and everything at or above 0xF0 must
 * report the same "Unknown exception type" string the kernel prints.
 *
 * tests/test_gpu_core.cpp and tests/test_mmu.cpp cover the two register read
 * paths that call this decode.
 */
#include "NativeMaliFault.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)

namespace {

const char *const kUnknown = "Unknown exception type";

struct Named {
    uint32_t code;
    const char *name;
};

/* Every named entry, in table order. */
const Named kNames[] = {
    {0x00, "OK"},
    {0x01, "DONE"},
    {0x02, "INTERRUPTED"},
    {0x03, "STOPPED"},
    {0x04, "TERMINATED"},
    {0x05, "KABOOM"},
    {0x06, "EUREKA"},
    {0x08, "ACTIVE"},
    {0x40, "JOB_CONFIG_FAULT"},
    {0x41, "JOB_POWER_FAULT"},
    {0x42, "JOB_READ_FAULT"},
    {0x43, "JOB_WRITE_FAULT"},
    {0x44, "JOB_AFFINITY_FAULT"},
    {0x48, "JOB_BUS_FAULT"},
    {0x50, "INSTR_INVALID_PC"},
    {0x51, "INSTR_INVALID_ENC"},
    {0x52, "INSTR_TYPE_MISMATCH"},
    {0x53, "INSTR_OPERAND_FAULT"},
    {0x54, "INSTR_TLS_FAULT"},
    {0x55, "INSTR_BARRIER_FAULT"},
    {0x56, "INSTR_ALIGN_FAULT"},
    {0x58, "DATA_INVALID_FAULT"},
    {0x59, "TILE_RANGE_FAULT"},
    {0x5A, "ADDR_RANGE_FAULT"},
    {0x5B, "IMPRECISE_FAULT"},
    {0x60, "OOM"},
    {0x61, "OOM_AFBC"},
    {0x7F, "UNKNOWN"},
    {0x80, "DELAYED_BUS_FAULT"},
    {0x88, "GPU_SHAREABILITY_FAULT"},
    {0x89, "SYS_SHAREABILITY_FAULT"},
    {0x8A, "GPU_CACHEABILITY_FAULT"},
    {0xC0, "TRANSLATION_FAULT_0"},
    {0xC1, "TRANSLATION_FAULT_1"},
    {0xC2, "TRANSLATION_FAULT_2"},
    {0xC3, "TRANSLATION_FAULT_3"},
    {0xC4, "TRANSLATION_FAULT_4"},
    {0xC7, "TRANSLATION_FAULT_IDENTITY"},
    {0xC8, "PERM_FAULT_0"},
    {0xC9, "PERM_FAULT_1"},
    {0xCA, "PERM_FAULT_2"},
    {0xCB, "PERM_FAULT_3"},
    {0xD0, "TRANSTAB_BUS_FAULT_0"},
    {0xD1, "TRANSTAB_BUS_FAULT_1"},
    {0xD2, "TRANSTAB_BUS_FAULT_2"},
    {0xD3, "TRANSTAB_BUS_FAULT_3"},
    {0xD8, "ACCESS_FLAG_0"},
    {0xD9, "ACCESS_FLAG_1"},
    {0xDA, "ACCESS_FLAG_2"},
    {0xDB, "ACCESS_FLAG_3"},
    {0xE0, "ADDR_SIZE_FAULT_IN0"},
    {0xE1, "ADDR_SIZE_FAULT_IN1"},
    {0xE2, "ADDR_SIZE_FAULT_IN2"},
    {0xE3, "ADDR_SIZE_FAULT_IN3"},
    {0xE4, "ADDR_SIZE_FAULT_OUT0"},
    {0xE5, "ADDR_SIZE_FAULT_OUT1"},
    {0xE6, "ADDR_SIZE_FAULT_OUT2"},
    {0xE7, "ADDR_SIZE_FAULT_OUT3"},
    {0xE8, "MEM_ATTR_FAULT_0"},
    {0xE9, "MEM_ATTR_FAULT_1"},
    {0xEA, "MEM_ATTR_FAULT_2"},
    {0xEB, "MEM_ATTR_FAULT_3"},
    {0xEC, "MEM_ATTR_NONCACHE_0"},
    {0xED, "MEM_ATTR_NONCACHE_1"},
    {0xEE, "MEM_ATTR_NONCACHE_2"},
    {0xEF, "MEM_ATTR_NONCACHE_3"},
};

size_t namedCount()
{
    return sizeof(kNames) / sizeof(kNames[0]);
}

const char *expectedName(uint32_t code)
{
    for(size_t i = 0; i < namedCount(); ++i)
        if(kNames[i].code == code)return kNames[i].name;
    return kUnknown;
}

/* The table must be exactly the pinned one: every named code returns its name,
 * every other byte returns the kernel's fallback. A stray or missing case in
 * the switch would show up here. */
void testExceptionNames()
{
    size_t matched = 0;
    for(uint32_t code = 0; code <= 0xFF; ++code) {
        const char *name = NativeMaliFault::exceptionName(code);
        CHECK(name != nullptr);
        CHECK(std::strcmp(name, expectedName(code)) == 0);
        if(std::strcmp(name, kUnknown) != 0)++matched;
    }
    CHECK(matched == namedCount());
    /* The holes the source really has, plus the end of the table. */
    CHECK(std::strcmp(NativeMaliFault::exceptionName(0x07), kUnknown) == 0);
    CHECK(std::strcmp(NativeMaliFault::exceptionName(0x09), kUnknown) == 0);
    CHECK(std::strcmp(NativeMaliFault::exceptionName(0x45), kUnknown) == 0);
    CHECK(std::strcmp(NativeMaliFault::exceptionName(0x7E), kUnknown) == 0);
    CHECK(std::strcmp(NativeMaliFault::exceptionName(0x8B), kUnknown) == 0);
    CHECK(std::strcmp(NativeMaliFault::exceptionName(0xC5), kUnknown) == 0);
    CHECK(std::strcmp(NativeMaliFault::exceptionName(0xCF), kUnknown) == 0);
    CHECK(std::strcmp(NativeMaliFault::exceptionName(0xD4), kUnknown) == 0);
    CHECK(std::strcmp(NativeMaliFault::exceptionName(0xDF), kUnknown) == 0);
    CHECK(std::strcmp(NativeMaliFault::exceptionName(0xEF), "MEM_ATTR_NONCACHE_3") == 0);
    CHECK(std::strcmp(NativeMaliFault::exceptionName(0xF0), kUnknown) == 0);
    /* exceptionName() takes the low byte only from the callers, but the
     * function itself is total: a code above 0xFF is out of table range. */
    CHECK(std::strcmp(NativeMaliFault::exceptionName(0x100), kUnknown) == 0);
    CHECK(std::strcmp(NativeMaliFault::exceptionName(0xFFFFFFFF), kUnknown) == 0);
}

void testAccessNames()
{
    CHECK(std::strcmp(NativeMaliFault::accessName(0), "ATOMIC") == 0);
    CHECK(std::strcmp(NativeMaliFault::accessName(1), "EXECUTE") == 0);
    CHECK(std::strcmp(NativeMaliFault::accessName(2), "READ") == 0);
    CHECK(std::strcmp(NativeMaliFault::accessName(3), "WRITE") == 0);
    /* Only the two access bits are looked at. */
    CHECK(std::strcmp(NativeMaliFault::accessName(0x6), "READ") == 0);
    CHECK(std::strcmp(NativeMaliFault::accessName(0xFFFFFFFF), "WRITE") == 0);
}

void testGpuFault()
{
    NativeMaliGpuFault fault;
    CHECK(fault.status == 0 && fault.addressLo == 0 && fault.addressHi == 0);
    CHECK(!fault.isFault());
    CHECK(fault.exception() == 0);
    CHECK(fault.address() == 0);

    fault.status = 0x1234C2;   /* only the low byte is the exception */
    fault.addressLo = 0xFFFFF000;
    fault.addressHi = 0x00000001;
    CHECK(fault.exception() == 0xC2);
    CHECK(fault.isFault());
    CHECK(std::strcmp(NativeMaliFault::exceptionName(fault.exception()),
        "TRANSLATION_FAULT_2") == 0);
    CHECK(fault.address() == UINT64_C(0x1FFFFF000));

    /* The boundary is MAX_NON_FAULT = 0x3F, not "any nonzero status". */
    NativeMaliGpuFault quiet;
    quiet.status = 0x08;
    CHECK(!quiet.isFault());
    quiet.status = 0x3F;
    CHECK(!quiet.isFault());
    quiet.status = 0x40;
    CHECK(quiet.isFault());
    quiet.status = 0xFF;
    CHECK(quiet.isFault());
}

void testMmuFault()
{
    NativeMaliMmuFault fault;
    CHECK(fault.status == 0 && fault.addressLo == 0 && fault.addressHi == 0);
    CHECK(!fault.isFault() && !fault.pageFault() && !fault.decoderFault());

    /* exception 0xC2, access READ (2 << 8), source id 0xABCD, decoder bit 10. */
    fault.status = (0x0000ABCDu << 16) | (1u << 10) | (2u << 8) | 0xC2;
    fault.addressLo = 0x00001000;
    fault.addressHi = 0x00000002;
    CHECK(fault.exception() == 0xC2);
    CHECK(fault.access() == 2);
    CHECK(std::strcmp(NativeMaliFault::accessName(fault.access()), "READ") == 0);
    CHECK(fault.sourceId() == 0xABCD);
    CHECK(fault.decoderFault());
    CHECK(fault.pageFault());
    CHECK(fault.isFault());
    CHECK(fault.address() == UINT64_C(0x200001000));

    /* Without the decoder bit it is a slave fault, still a page fault. */
    fault.status &= ~(1u << 10);
    CHECK(!fault.decoderFault());
    CHECK(fault.pageFault());

    /* Page faults are exception types 0xC0..0xC7, whatever the access bits. */
    for(uint32_t exception = 0xC0; exception <= 0xC7; ++exception) {
        fault.status = (3u << 8) | exception;
        CHECK(fault.pageFault());
        CHECK(fault.access() == 3);
        CHECK(fault.isFault());
    }
    fault.status = 0xC8;   /* PERM_FAULT_0 is terminal */
    CHECK(!fault.pageFault());
    fault.status = 0xBF;   /* just below the translation range */
    CHECK(!fault.pageFault());
    fault.status = 0x40;
    CHECK(!fault.pageFault());
    CHECK(fault.isFault());

    /* The source keeps the whole upper word as the source id. */
    fault.status = 0xFFFF0040;
    CHECK(fault.sourceId() == 0xFFFF);
    CHECK(fault.exception() == 0x40);
    CHECK(fault.access() == 0);
    CHECK(std::strcmp(NativeMaliFault::accessName(fault.access()), "ATOMIC") == 0);
}

} // namespace

int main()
{
    testExceptionNames();
    testAccessNames();
    testGpuFault();
    testMmuFault();
    std::puts("PASS: fault exception/access decode against the pinned panfrost table, sparse holes, address assembly, page-fault and decoder bits");
    return 0;
}
