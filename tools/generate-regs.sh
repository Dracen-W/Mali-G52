#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Generate NativeMaliRegs.h from the pinned panfrost register header.
#
#   tools/generate-regs.sh [output]      (default: NativeMaliRegs.h)
#
# The definitions are copied verbatim, so every offset in the generated header is
# the source's own line and nothing is retyped. The script refuses to run if the
# source no longer matches the sha256 in third_party/PINNED.md: a changed source
# file means the citations in the driver may be wrong and a human has to look.
#
# tests/run.sh regenerates into build/ and compares it with the checked-in file,
# so the header cannot drift from the source it claims to come from.
set -eu
cd "$(dirname "$0")/.."

source_path=third_party/linux-panfrost/drivers/gpu/drm/panfrost/panfrost_regs.h
output=${1:-NativeMaliRegs.h}

if [ ! -f "$source_path" ]; then
    echo "missing $source_path" >&2
    exit 1
fi

# The expected hash lives in PINNED.md so there is one place to update.
expected=$(sed -n 's/^| `\([0-9a-f]\{64\}\)` | `.*panfrost_regs\.h` |$/\1/p' third_party/PINNED.md)
if [ -z "$expected" ]; then
    echo "no pin for panfrost_regs.h in third_party/PINNED.md" >&2
    exit 1
fi
if command -v shasum >/dev/null 2>&1; then
    actual=$(shasum -a 256 "$source_path" | cut -d' ' -f1)
else
    actual=$(sha256sum "$source_path" | cut -d' ' -f1)
fi
if [ "$actual" != "$expected" ]; then
    echo "$source_path has changed since it was pinned" >&2
    echo "  pinned: $expected" >&2
    echo "  actual: $actual" >&2
    echo "update third_party/PINNED.md and re-check the citations first" >&2
    exit 1
fi

mkdir -p "$(dirname "$output")"
{
    cat <<HEADER
/* SPDX-License-Identifier: GPL-2.0-only */
/* GENERATED FILE - do not edit. Regenerate with tools/generate-regs.sh.
 *
 * Source: $source_path
 * sha256: $expected   (the pin is third_party/PINNED.md)
 *
 * Every definition below is that file's own line, copied verbatim: the register
 * map of the pinned panfrost driver, whose copyright is ARM Limited, Linaro and
 * Marty E. Plummer, as stated in the source. The generator refuses to run when
 * the source no longer matches the pinned hash, so these offsets cannot drift
 * from the file the driver cites. tests/run.sh regenerates this file into build/
 * and compares, and tests/test_regs.cpp locks the values the driver depends on.
 *
 * BIT, GENMASK and GENMASK_ULL are the kernel's 64-bit forms, supplied here
 * because the source uses them: several definitions need bits above 31
 * (AS_TRANSCFG_XREADABLE is BIT(36)), which a 32-bit BIT would get wrong.
 * They are only defined when the including file has not defined them already.
 */
#ifndef NATIVE_MALI_REGS_H
#define NATIVE_MALI_REGS_H

#ifndef BIT
#define BIT(nr) (1UL << (nr))
#endif
#ifndef GENMASK
#define GENMASK(h, l) (((~0UL) << (l)) & (~0UL >> (63 - (h))))
#endif
#ifndef GENMASK_ULL
#define GENMASK_ULL(h, l) (((~0ULL) << (l)) & (~0ULL >> (63 - (h))))
#endif

HEADER
    # Skip the source's own include guard and the two kernel I/O helpers: they
    # reference a struct panfrost_device and cannot travel. Everything else is
    # copied as is, including the comments that explain a register.
    awk '
        /^#ifndef __PANFROST_REGS_H__[[:space:]]*$/ { next }
        /^#define __PANFROST_REGS_H__[[:space:]]*$/ { emit = 1; next }
        /^#endif[[:space:]]*$/ { next }
        /^#define[[:space:]]+gpu_(read|write)\(/ { next }
        emit { print }
    ' "$source_path"
    printf '\n#endif /* NATIVE_MALI_REGS_H */\n'
} > "$output"

printf 'generated %s from %s (%s)\n' "$output" "$source_path" "$expected"
