#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Build and run every host test. Nothing here touches hardware.
#
#   tests/run.sh            normal build
#   tests/run.sh --san      same tests under AddressSanitizer + UBSan, which is
#                           where a wrong size, an off-by-one page or an unaligned
#                           table access shows up as a failure instead of as a
#                           silent wrong number
#   CXX=clang++ tests/run.sh
set -eu
cd "$(dirname "$0")/.."
CXX=${CXX:-c++}
CXXFLAGS=${CXXFLAGS:--std=c++20 -O1 -g -Wall -Wextra}
# The M2/M3/M4/M6 write paths are compiled out by default; the tests are the
# harness that exercises them, so they are the one build that opts in. The
# driver itself never sets these (docs/TESTING.md).
CXXFLAGS="$CXXFLAGS -DNATIVE_MALI_M2_WRITES=1"
CXXFLAGS="$CXXFLAGS -DNATIVE_MALI_M3_WRITES=1"
CXXFLAGS="$CXXFLAGS -DNATIVE_MALI_M4_WRITES=1"
CXXFLAGS="$CXXFLAGS -DNATIVE_MALI_M6_WRITES=1"
if [ "${1:-}" = "--san" ]; then
    CXXFLAGS="$CXXFLAGS -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
fi
OUT=build/host
mkdir -p "$OUT"

# The register map is generated, so the checked-in copy must still be what the
# pinned source produces. This fails before any test runs if it is stale.
printf 'check  NativeMaliRegs.h\n'
sh tools/generate-regs.sh "$OUT/NativeMaliRegs.h" >/dev/null
if ! cmp -s "$OUT/NativeMaliRegs.h" NativeMaliRegs.h; then
    echo "NativeMaliRegs.h is stale: run tools/generate-regs.sh" >&2
    exit 1
fi

# The IOKit glue cannot be built here: there is no XNU header set on a host.
# The local stubs are a syntax gate only (tests/xnu-stub/README.md); the real
# compile is build-arm-driver.py against the kernel tree.
printf 'check  xnu glue (stub headers, syntax only)\n'
$CXX $CXXFLAGS -Wno-overloaded-virtual -fsyntax-only -Itests/xnu-stub -I. \
    xnu/NativeMaliGpuService.cpp xnu/NativeMaliXnuSupport.cpp

# The driver sources under test. Keep this a glob of the real source files so a
# new file cannot be forgotten by the harness.
SOURCES=""
for source in NativeMali*.cpp; do
    SOURCES="$SOURCES $source"
done

for source in tests/test_*.cpp; do
    name=$(basename "$source" .cpp)
    printf 'build  %s\n' "$name"
    $CXX $CXXFLAGS -I. -Itests -Itests/shim "$source" $SOURCES \
        -o "$OUT/$name"
    printf 'run    %s\n' "$name"
    "./$OUT/$name" tests/data/mt6768.dtb
done
printf 'all host tests passed\n'
