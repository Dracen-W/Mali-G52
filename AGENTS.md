# Agent instructions

## Responsibility

Maintain a native XNU/IOKit driver for the MediaTek MT6768 Mali-G52 MC2
(Bifrost v7, Job Manager). The goal is a working GPU without firmware or a GMU:
register bring-up, ARM_MALI_LPAE page tables, job-slot submission with software
fences, and a fixed user-client ABI in the shape of NativeAdreno.

## Required reading, in order

1. `README.md` — current milestone state and the module map.
2. `docs/TODO.md` — what is deliberately not implemented and why.
3. `docs/OPEN_QUESTIONS.md` — what is waiting on a human answer or on hardware.
4. `docs/TESTING.md` — what the host tests actually prove.
5. `docs/DEVICE.md` — measured facts about this phone, with the raw lines.
6. `docs/MTK_SOURCES.md` — where the vendor values come from and their hashes.
7. `docs/KEXT_BUILD.md` — what a real kernel build needs, built-in vs kext, and
   what is still missing from the environment.
8. The milestone roadmap in `workfile/` (development file, **never commit**).

## Non-negotiable rules

- **Every register offset, bit and magic value cites its source.** Use
  `path:function` or `path:line` plus the pinned hash from
  `third_party/PINNED.md` or `docs/MTK_SOURCES.md`. Never write a value from
  memory, and never copy an offset from a different SoC.
- **Read before write.** Power/clock/PMIC/MMU code starts as a read-only probe.
  Configuration writes stay behind `NATIVE_MALI_M2_WRITES` (default `0`) until a
  human has accepted the probe output on the device. Do not "test it quickly" by
  defining the flag in a driver build.
- **Three layers.** Pure logic with injected `NativeMaliRegisterTarget` IO; IOKit
  is glue only. New logic must be host-testable without hardware.
- **Tests first.** Add the failing test, then the code. `tests/run.sh` must pass,
  and `tests/run.sh --san` must pass for anything touching memory.
- **Fail closed.** On error: quiesce, roll back in reverse order, return the
  original error. No retries that hide a failure, no swallowed errors, no
  reporting ready after a partial bring-up.
- **Voltage is fixed and hard-limited.** 500000–1293750 µV, exact 6250 µV steps.
  No automatic DVFS. The fixed values are the lowest vendor OPP.
- **DTB wins over the roadmap** when they disagree (for example interrupt
  polarity is level low, `0x8`). Record the conflict and the decision in
  `docs/OPEN_QUESTIONS.md`.
- **Licences.** New files are `GPL-2.0-only`. `NativeMaliHandleIndex.h` is a
  literal MIT port and keeps its MIT header. Update `NOTICE` when the set of
  derived files changes.

## Build and verification

```sh
tests/run.sh          # build and run every host test
tests/run.sh --san    # same, AddressSanitizer + UBSan
CXX=clang++ tests/run.sh
```

`tests/run.sh` compiles every root `NativeMali*.cpp` and every
`tests/test_*.cpp`, so a new module or test is picked up automatically. It
regenerates `NativeMaliRegs.h` and fails if the checked-in copy is stale.

There is no kext build yet: the XNU-side service and `Info.plist` are waiting on
the platform questions in `docs/OPEN_QUESTIONS.md`. Do not add a plist that
names an IOClass that does not exist.

## Commits

- Commit in development order, one coherent step at a time.
- **Never commit** `workfile/`, `Adreno740-main/`, `NativeAdreno-reference-old/`,
  `third_party/mtk-android/`, `other reference/`, or any other reference copy.
  They are development inputs, not deliverables.
- After each commit, state exactly which files were committed.
- Do not push; the owner reviews first.

## When unsure

Stop. Write the question and the evidence in `docs/OPEN_QUESTIONS.md`, mark the
code path `Unsupported`/`NotReady`, and continue with a part that is certain.
A missing feature is recoverable; a guessed register value is not.
