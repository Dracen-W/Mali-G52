/* SPDX-License-Identifier: GPL-2.0-only */
/* The wire ABI's layout and the checks that need no client state.
 *
 * Layout: the static_asserts in NativeMaliABI.h already fix every size at
 * compile time; the offsetof goldens here fix the packing too, so a field
 * inserted in the middle cannot silently move the ones after it.
 *
 * Checks: every refusal path the roadmap's M7 acceptance asks for, as far as one
 * record can decide it - malformed size/alignment, unknown version, method or
 * flag bit, a range outside the 48-bit address space, a circular timeout, a
 * fence this driver could not have produced. Handle existence, fence ownership
 * and IOVA collisions need the client's tables and are not in here. */
#include "NativeMaliABI.h"
#include "NativeMaliAbiValidate.h"
#include <cstddef>
#include <cstdio>
#include <initializer_list>
#include <cstdlib>

#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)

namespace {

void testLayout()
{
    /* Sizes are static_asserted; pin the offsets so padding cannot move. */
    CHECK(offsetof(NativeMaliAbiQuery, abi_version) == 0);
    CHECK(offsetof(NativeMaliAbiQuery, flags) == 12);
    CHECK(offsetof(NativeMaliAbiQuery, max_bo_bytes) == 16);

    CHECK(offsetof(NativeMaliAbiDeviceInfo, gpu_id) == 0);
    CHECK(offsetof(NativeMaliAbiDeviceInfo, shader_present_hi) == 8);
    CHECK(offsetof(NativeMaliAbiDeviceInfo, flags) == 44);
    CHECK(offsetof(NativeMaliAbiDeviceInfo, va_start) == 48);
    CHECK(offsetof(NativeMaliAbiDeviceInfo, va_size) == 56);

    CHECK(offsetof(NativeMaliAbiBoMap, iova) == 8);
    CHECK(offsetof(NativeMaliAbiBoMap, permissions) == 16);
    CHECK(offsetof(NativeMaliAbiBoMap, reserved) == 20);

    CHECK(offsetof(NativeMaliAbiBoCpuMapReply, bytes) == 8);
    CHECK(offsetof(NativeMaliAbiBoCpuUnmap, token) == 8);

    CHECK(offsetof(NativeMaliAbiSubmit, wait_fence) == 8);
    CHECK(offsetof(NativeMaliAbiSubmit, wait_timeout_ns) == 16);
    CHECK(offsetof(NativeMaliAbiSubmit, flags) == 24);
    CHECK(offsetof(NativeMaliAbiSubmit, reserved) == 28);

    /* The constants the driver derives them from (NativeMaliVm.h). */
    CHECK(NativeMaliAbiPageBytes == 4096);
    CHECK(NativeMaliAbiJobChainAlign == 64);
    CHECK(NATIVE_MALI_ABI_MAX_BO_BYTES ==
        (UINT64_C(64) * 512 * NativeMaliAbiPageBytes));
    CHECK(NATIVE_MALI_ABI_MAX_BO_BYTES == (UINT64_C(1) << 27));
    CHECK(NativeMaliBoRead == 1u && NativeMaliBoWrite == 2u);
    CHECK(NativeMaliDeviceNonCoherent == 1u);
}

void testVersionAndMethod()
{
    CHECK(NativeMaliAbiValidate::version(NativeMaliAbiVersion) ==
        kIOReturnSuccess);
    CHECK(NativeMaliAbiValidate::version(0) == kIOReturnUnsupported);
    CHECK(NativeMaliAbiValidate::version(NativeMaliAbiVersion + 1) ==
        kIOReturnUnsupported);
    CHECK(NativeMaliAbiValidate::version(0xFFFFFFFFu) ==
        kIOReturnUnsupported);

    for(uint32_t method = NativeMaliMethodQuery;
        method <= NativeMaliMethodLast; ++method)
        CHECK(NativeMaliAbiValidate::method(method) == kIOReturnSuccess);
    CHECK(NativeMaliAbiValidate::method(0) == kIOReturnUnsupported);
    CHECK(NativeMaliAbiValidate::method(NativeMaliMethodLast + 1) ==
        kIOReturnUnsupported);
    CHECK(NativeMaliAbiValidate::method(0xFFFFFFFFu) ==
        kIOReturnUnsupported);

    CHECK(NativeMaliMethodQuery == 1);
    CHECK(NativeMaliMethodGetStatus == 10);
    CHECK(NativeMaliMethodReset == 11);
    CHECK(NativeMaliMethodLast == NativeMaliMethodReset);

    NativeMaliAbiQuery query{};
    query.abi_version = NativeMaliAbiVersion;
    CHECK(NativeMaliAbiValidate::query(query) == kIOReturnSuccess);
    /* The reply fields are the service's to fill; a caller's zeros are fine. */
    query.input_bits = 0;
    query.output_bits = 0;
    query.max_bo_bytes = 0;
    CHECK(NativeMaliAbiValidate::query(query) == kIOReturnSuccess);
    query.abi_version = 0;
    CHECK(NativeMaliAbiValidate::query(query) == kIOReturnUnsupported);
    query.abi_version = NativeMaliAbiVersion;
    query.flags = 1;
    CHECK(NativeMaliAbiValidate::query(query) == kIOReturnUnsupported);
    query.flags = 0;
    query.abi_version = NativeMaliAbiVersion + 1;
    CHECK(NativeMaliAbiValidate::query(query) == kIOReturnUnsupported);
}

void testBoAlloc()
{
    const uint64_t max = NATIVE_MALI_ABI_MAX_BO_BYTES;
    for(uint64_t bytes : {UINT64_C(0), UINT64_C(1), UINT64_C(4095),
        UINT64_C(4097), UINT64_C(8191)})
    {
        NativeMaliAbiBoAlloc alloc{bytes};
        CHECK(NativeMaliAbiValidate::boAlloc(alloc, max) ==
            kIOReturnBadArgument);
    }
    for(uint64_t bytes : {UINT64_C(4096), UINT64_C(8192), max})
    {
        NativeMaliAbiBoAlloc alloc{bytes};
        CHECK(NativeMaliAbiValidate::boAlloc(alloc, max) == kIOReturnSuccess);
    }
    /* One page over the advertised maximum: well formed, cannot be satisfied. */
    {
        NativeMaliAbiBoAlloc alloc{max + 4096};
        CHECK(NativeMaliAbiValidate::boAlloc(alloc, max) ==
            kIOReturnNoResources);
    }
    /* A zero maximum means "no limit from the caller" (the service always
     * passes one, this keeps the helper usable alone). */
    {
        NativeMaliAbiBoAlloc alloc{UINT64_C(1) << 40};
        CHECK(NativeMaliAbiValidate::boAlloc(alloc, 0) == kIOReturnSuccess);
    }
    /* Wrapping the multiplication a client might try is still a page multiple
     * check, so an overflowing size is either rejected or bounded above. */
    {
        NativeMaliAbiBoAlloc alloc{UINT64_C(0xFFFFFFFFFFFFF000)};
        CHECK(NativeMaliAbiValidate::boAlloc(alloc, max) ==
            kIOReturnNoResources);
    }
}

void testBoFreeAndCpuMap()
{
    CHECK(NativeMaliAbiValidate::boFree(NativeMaliAbiBoFree{0}) ==
        kIOReturnBadArgument);
    CHECK(NativeMaliAbiValidate::boFree(NativeMaliAbiBoFree{1}) ==
        kIOReturnSuccess);

    CHECK(NativeMaliAbiValidate::boCpuMap(NativeMaliAbiBoCpuMap{0}) ==
        kIOReturnBadArgument);
    CHECK(NativeMaliAbiValidate::boCpuMap(NativeMaliAbiBoCpuMap{7}) ==
        kIOReturnSuccess);

    CHECK(NativeMaliAbiValidate::boCpuUnmap(
        NativeMaliAbiBoCpuUnmap{7, 0, 0}) == kIOReturnSuccess);
    CHECK(NativeMaliAbiValidate::boCpuUnmap(
        NativeMaliAbiBoCpuUnmap{0, 0, 0}) == kIOReturnBadArgument);
    CHECK(NativeMaliAbiValidate::boCpuUnmap(
        NativeMaliAbiBoCpuUnmap{7, 0, 1}) == kIOReturnBadArgument);
}

void testBoMap()
{
    const uint64_t boBytes = 0x4000;
    NativeMaliAbiBoMap map{};
    map.handle = 1;
    map.iova = 0x10000;
    map.permissions = NativeMaliBoRead | NativeMaliBoWrite;
    CHECK(NativeMaliAbiValidate::boMap(map, boBytes) == kIOReturnSuccess);

    map.permissions = NativeMaliBoRead;
    CHECK(NativeMaliAbiValidate::boMap(map, boBytes) == kIOReturnSuccess);

    /* Handle, reserved, address and permission refusals. */
    map.handle = 0;
    CHECK(NativeMaliAbiValidate::boMap(map, boBytes) == kIOReturnBadArgument);
    map.handle = 1;
    map.reserved = 1;
    CHECK(NativeMaliAbiValidate::boMap(map, boBytes) == kIOReturnBadArgument);
    map.reserved = 0;
    map.iova = 0;
    CHECK(NativeMaliAbiValidate::boMap(map, boBytes) == kIOReturnBadArgument);
    map.iova = 0x10800;
    CHECK(NativeMaliAbiValidate::boMap(map, boBytes) == kIOReturnBadArgument);
    map.iova = 0x10000;
    map.permissions = 0;
    CHECK(NativeMaliAbiValidate::boMap(map, boBytes) == kIOReturnBadArgument);
    map.permissions = NativeMaliBoWrite;
    CHECK(NativeMaliAbiValidate::boMap(map, boBytes) == kIOReturnBadArgument);
    map.permissions = NativeMaliBoRead | (1u << 2);
    CHECK(NativeMaliAbiValidate::boMap(map, boBytes) == kIOReturnUnsupported);
    map.permissions = NativeMaliBoRead | 0x80000000u;
    CHECK(NativeMaliAbiValidate::boMap(map, boBytes) == kIOReturnUnsupported);

    /* The size is not in the record, so it is validated from the caller's
     * side: it must be a real page count. */
    map.permissions = NativeMaliBoRead;
    CHECK(NativeMaliAbiValidate::boMap(map, 0) == kIOReturnBadArgument);
    CHECK(NativeMaliAbiValidate::boMap(map, 4097) == kIOReturnBadArgument);

    /* The range must stay inside the 48-bit space, and x + size must not wrap. */
    const uint64_t vaSize = UINT64_C(1) << 48;
    map.iova = vaSize - boBytes;
    CHECK(NativeMaliAbiValidate::boMap(map, boBytes) == kIOReturnSuccess);
    map.iova = vaSize;
    CHECK(NativeMaliAbiValidate::boMap(map, boBytes) == kIOReturnBadArgument);
    map.iova = vaSize - 4096;
    CHECK(NativeMaliAbiValidate::boMap(map, boBytes) == kIOReturnBadArgument);
    map.iova = 0xFFFFFFFFFFFFF000u;
    CHECK(NativeMaliAbiValidate::boMap(map, boBytes) == kIOReturnBadArgument);
    map.iova = 0;
    CHECK(NativeMaliAbiValidate::boMap(map, vaSize) == kIOReturnBadArgument);
}

void testSubmit()
{
    NativeMaliAbiSubmit submit{};
    submit.job_chain = 0x1000;
    CHECK(NativeMaliAbiValidate::submit(submit) == kIOReturnSuccess);

    /* A chain address the hardware cannot read as a job header. */
    submit.job_chain = 0;
    CHECK(NativeMaliAbiValidate::submit(submit) == kIOReturnBadArgument);
    for(uint64_t chain : {UINT64_C(8), UINT64_C(63), UINT64_C(65),
        UINT64_C(0x10000001)})
    {
        submit.job_chain = chain;
        CHECK(NativeMaliAbiValidate::submit(submit) == kIOReturnBadArgument);
    }
    submit.job_chain = UINT64_C(0xFFFFFFFFFFC0);
    CHECK(NativeMaliAbiValidate::submit(submit) == kIOReturnSuccess);

    /* Unknown flag bits are a newer client; reserved bytes are malformed. */
    submit.flags = 1;
    CHECK(NativeMaliAbiValidate::submit(submit) == kIOReturnUnsupported);
    submit.flags = 0;
    submit.reserved = 1;
    CHECK(NativeMaliAbiValidate::submit(submit) == kIOReturnBadArgument);
    submit.reserved = 0;

    /* A timeout without a fence means nothing, so it is refused rather than
     * ignored. */
    submit.wait_timeout_ns = 1000;
    CHECK(NativeMaliAbiValidate::submit(submit) == kIOReturnBadArgument);
    submit.wait_timeout_ns = 0;

    /* A fence with a zero seqno is not one this driver ever produced. */
    submit.wait_fence = UINT64_C(1) << 32;
    CHECK(NativeMaliAbiValidate::submit(submit) == kIOReturnBadArgument);
    submit.wait_fence = NativeMaliAbiValidate::packFence(0, 1);
    CHECK(NativeMaliAbiValidate::submit(submit) == kIOReturnSuccess);
    submit.wait_timeout_ns = 5000000;
    CHECK(NativeMaliAbiValidate::submit(submit) == kIOReturnSuccess);
}

void testWaitFence()
{
    CHECK(NativeMaliAbiValidate::waitFence(NativeMaliAbiWaitFence{0, 0}) ==
        kIOReturnBadArgument);
    CHECK(NativeMaliAbiValidate::waitFence(NativeMaliAbiWaitFence{1, 0}) ==
        kIOReturnSuccess);
    CHECK(NativeMaliAbiValidate::waitFence(NativeMaliAbiWaitFence{1, 1000}) ==
        kIOReturnSuccess);
    /* The packed form the service hands out, including its zero-seqno hole. */
    CHECK(NativeMaliAbiValidate::waitFence(NativeMaliAbiWaitFence{
        NativeMaliAbiValidate::packFence(2, 7), 0}) == kIOReturnSuccess);
    CHECK(NativeMaliAbiValidate::waitFence(NativeMaliAbiWaitFence{
        NativeMaliAbiValidate::packFence(2, 0), 0}) == kIOReturnBadArgument);
}

void testFenceEncoding()
{
    const uint64_t fence = NativeMaliAbiValidate::packFence(1, 3);
    CHECK(fence == ((UINT64_C(1) << 32) | 3));
    CHECK(NativeMaliAbiValidate::fenceSlot(fence) == 1);
    CHECK(NativeMaliAbiValidate::fenceSeqno(fence) == 3);
    /* Round trip over every slot and a few seqnos, including the 32-bit edge. */
    for(uint32_t slot = 0; slot < 3; ++slot)
        for(uint32_t seqno : {1u, 2u, 0xFFFFFFFFu}) {
            const uint64_t packed = NativeMaliAbiValidate::packFence(slot, seqno);
            CHECK(NativeMaliAbiValidate::fenceSlot(packed) == slot);
            CHECK(NativeMaliAbiValidate::fenceSeqno(packed) == seqno);
        }
    /* Slot 0's first job is 1, never 0: that is what makes 0 "no fence". */
    CHECK(NativeMaliAbiValidate::packFence(0, 0) == 0);
    CHECK(NativeMaliAbiValidate::packFence(0, 1) != 0);
}

} // namespace

int main()
{
    testLayout();
    testVersionAndMethod();
    testBoAlloc();
    testBoFreeAndCpuMap();
    testBoMap();
    testSubmit();
    testWaitFence();
    testFenceEncoding();
    std::puts("PASS: Mali wire ABI layout goldens and the pure user-client checks "
        "(version/method/flag refusal, page-multiple and maximum BO sizes, "
        "aligned in-range IOVA, read-required permissions, 64-byte chain "
        "alignment, no timeout without a fence, no zero-seqno fence)");
    return 0;
}
