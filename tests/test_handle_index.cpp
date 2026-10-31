/* SPDX-License-Identifier: GPL-2.0-only */
/* NativeMaliHandleIndex: the shared open-addressing template the M7 user client
 * will use. Ported from NativeAdreno (MIT, see NOTICE), so the test checks the
 * two properties that port depends on: a probe run survives deletion of an
 * earlier member (backward shift, no tombstones) and the table never reports a
 * stale value for a key that is gone. */
#include "NativeMaliHandleIndex.h"
#include <cstdio>
#include <cstdlib>

#define CHECK(x) do {if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)

namespace {

/* Same expression as the template's private home(), restated so the test can
 * pick keys that collide without exposing the internals. */
template<size_t Slots> size_t homeOf(uint64_t key)
{
    constexpr size_t size = Slots * 2;
    return static_cast<size_t>(
        (key * UINT64_C(0x9e3779b97f4a7c15)) >> 40) & (size - 1);
}

void testLifecycle()
{
    NativeMaliHandleIndex<8> index;
    CHECK(index.find(1) == UINT32_MAX);
    index.insert(1, 1);          /* ignored before allocate() */
    CHECK(index.find(1) == UINT32_MAX);
    index.erase(1);              /* also ignored */
    CHECK(index.allocate());
    CHECK(index.find(1) == UINT32_MAX);

    CHECK(index.allocate());     /* re-allocate clears the table */
    index.insert(7, 70);
    CHECK(index.find(7) == 70);
    CHECK(index.allocate());
    CHECK(index.find(7) == UINT32_MAX);

    index.insert(7, 70);
    index.release();
    CHECK(index.find(7) == UINT32_MAX);
    index.release();             /* double release is harmless */
}

void testInsertFind()
{
    NativeMaliHandleIndex<4> index;   /* 8 cells, 4 keys */
    CHECK(index.allocate());
    for(uint64_t key = 1; key <= 4; ++key)
        index.insert(key, static_cast<uint32_t>(key * 3));
    for(uint64_t key = 1; key <= 4; ++key)
        CHECK(index.find(key) == key * 3);
    CHECK(index.find(5) == UINT32_MAX);
    CHECK(index.find(0) == UINT32_MAX);

    /* Re-inserting a key replaces the value instead of taking a second cell. */
    index.insert(3, 99);
    CHECK(index.find(3) == 99);
    CHECK(index.find(1) == 3);

    /* Key 0 is the empty marker and must never be stored. */
    index.insert(0, 1234);
    CHECK(index.find(0) == UINT32_MAX);
    for(uint64_t key = 1; key <= 4; ++key)
        CHECK(index.find(key) != UINT32_MAX);
}

void testErase()
{
    NativeMaliHandleIndex<4> index;
    CHECK(index.allocate());
    for(uint64_t key = 1; key <= 4; ++key)
        index.insert(key, static_cast<uint32_t>(key));
    index.erase(2);
    CHECK(index.find(2) == UINT32_MAX);
    CHECK(index.find(1) == 1);
    CHECK(index.find(3) == 3);
    CHECK(index.find(4) == 4);

    index.erase(2);              /* erasing an absent key changes nothing */
    index.erase(0);
    CHECK(index.find(1) == 1);
    CHECK(index.find(3) == 3);

    for(uint64_t key = 1; key <= 4; ++key)
        index.erase(key);
    for(uint64_t key = 1; key <= 4; ++key)
        CHECK(index.find(key) == UINT32_MAX);

    /* A slot freed by the last erase must be reusable. */
    index.insert(4, 40);
    CHECK(index.find(4) == 40);
}

/* Three keys whose home cell is the same. Linear probing puts them at
 * home, home+1, home+2; erasing the first must shift the other two back or
 * the lookup would stop at the empty cell it left behind. */
void testBackwardShift()
{
    constexpr size_t kSlots = 4;
    uint64_t keys[3] = {0, 0, 0};
    size_t found = 0;
    const size_t wanted = homeOf<kSlots>(1);
    for(uint64_t key = 1; key < 100000 && found < 3; ++key)
        if(homeOf<kSlots>(key) == wanted)
            keys[found++] = key;
    CHECK(found == 3);

    NativeMaliHandleIndex<kSlots> index;
    CHECK(index.allocate());
    for(size_t i = 0; i < 3; ++i)
        index.insert(keys[i], static_cast<uint32_t>(100 + i));
    for(size_t i = 0; i < 3; ++i)
        CHECK(index.find(keys[i]) == 100 + i);

    index.erase(keys[0]);
    CHECK(index.find(keys[0]) == UINT32_MAX);
    CHECK(index.find(keys[1]) == 101);
    CHECK(index.find(keys[2]) == 102);

    index.erase(keys[1]);
    CHECK(index.find(keys[2]) == 102);

    /* Refill the hole and confirm the shifted cells are still findable. */
    index.insert(keys[0], 200);
    CHECK(index.find(keys[0]) == 200);
    CHECK(index.find(keys[2]) == 102);
}

/* The same run spanning the end of the array: erase a cell near the top so the
 * backward shift has to wrap. */
void testBackwardShiftWraps()
{
    constexpr size_t kSlots = 4;
    constexpr size_t kSize = kSlots * 2;
    uint64_t keys[3] = {0, 0, 0};
    size_t found = 0;
    for(uint64_t key = 1; key < 100000 && found < 3; ++key)
        if(homeOf<kSlots>(key) == kSize - 2)
            keys[found++] = key;
    CHECK(found == 3);

    NativeMaliHandleIndex<kSlots> index;
    CHECK(index.allocate());
    for(size_t i = 0; i < 3; ++i)
        index.insert(keys[i], static_cast<uint32_t>(i + 1));
    index.erase(keys[0]);
    CHECK(index.find(keys[0]) == UINT32_MAX);
    CHECK(index.find(keys[1]) == 2);
    CHECK(index.find(keys[2]) == 3);
}

void testDistinctValues()
{
    NativeMaliHandleIndex<16> index;
    CHECK(index.allocate());
    for(uint64_t key = 1; key <= 16; ++key)
        index.insert(key * 0x100000001u, static_cast<uint32_t>(key));
    for(uint64_t key = 1; key <= 16; ++key)
        CHECK(index.find(key * 0x100000001u) == key);
    CHECK(index.find(0x100000001u * 17) == UINT32_MAX);
}

} // namespace

int main()
{
    testLifecycle();
    testInsertFind();
    testErase();
    testBackwardShift();
    testBackwardShiftWraps();
    testDistinctValues();
    std::puts("PASS: handle index allocate/release, insert/replace, key-0 refusal, erase, backward shift (including wrap) and full-table lookup");
    return 0;
}
