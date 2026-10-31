# NativeMali

Native XNU driver for the MediaTek MT6768 Mali-G52 MC2 (Bifrost v7, Job Manager).

No firmware, no GMU, no command processor ring: register bring-up, the GPU's own
MMU (ARM_MALI_LPAE page tables), job-slot submission with software fences, and a
fixed IOUserClient ABI in the shape of Adreno740 (formerly NativeAdreno).

Licence: GPL-2.0-only (see `LICENSE` and `NOTICE`).

## State

Nothing is published to the kernel yet. The driver must refuse to report ready
until every step of a milestone has passed; a failed step quiesces and stops.

| Milestone | State |
| --- | --- |
| M0 repository, pinned sources, host tests | done |
| M1 platform recon (device tree, read-only probe) | device-tree parsing done; on-device probe needs the XNU-side service |
| M2 power, clocks, regulators | read-only probe, fixed regulators, clk26m mux, the four MFG domains and the reverse teardown (clock gate then domains) written and fake-register tested; writes compile out by default; on-device acceptance open |
| M3 GPU core | register map generated and locked; identity decode done; read-only feature probe behind the M2 gate; soft/hard reset, the G52 quirk write and L2/shader/tiler power on/off behind `NATIVE_MALI_M3_WRITES`; on-device acceptance open |
| M4 MMU and page tables | ARM_MALI_LPAE page tables built and host-tested; AS slot allocation (reset after a GPU reset), MMU enable/disable, AS fault decoding, range `FLUSH_PT` and the non-coherent page-table publish behind `NATIVE_MALI_M4_WRITES`; MMU interrupt path open |
| M5 memory objects and cache maintenance | non-coherent decision recorded; spans, mapping through `NativeMaliMmu::flushRange()`, the clean/invalidate policy and the injected cache interface host-tested; the XNU allocator and cache primitives are open |
| M6 jobs | the Bifrost v7 job header and WRITE_VALUE payload packed from the pinned Mesa XML in 64-byte slots with a Next chain, the `panfrost_job_hw_submit()` slot handshake, a polled `JOB_INT_RAWSTAT` completion wait and per-slot software fence seqnos, all behind `NATIVE_MALI_M6_WRITES` and host-tested; interrupt-driven completion notification open |
| M7 UserClient and ABI | the fixed-size wire ABI (`NativeMaliABI.h`), its pure checks, the BO half of the client state machine (`NativeMaliBoTable`), the rest of it (`NativeMaliUserClient`: Query/GetDeviceInfo, submit with slot choice and flush id, own-fence bookkeeping, dependency waits, polled completion, fail-closed close/teardown, and the device-wide Reset selector forwarded to the service) and the device-wide owner (`NativeMaliService`: one AS per open client, start/stop gates, the reset with its job-manager quiesce and counted soft-stop timeout that reprograms every client AS and reports destroyed fences) are written and host-tested; only the IOUserClient subclass and its `Info.plist` wait on the XNU capabilities |
| M8..M10 | not started |

Cross-milestone: `NativeMaliBringup` runs M1 → M2 → M3 → M4 in one call in the
order panfrost uses (read the platform, power up, identify and reset the core,
then record `GPU_AS_PRESENT` and clear stale allocations) and undoes it in
reverse. Its order and rollback are host-tested across blocks. `NativeMaliService`
is the object above it that owns the shared job slots and gives each open client
its own address space, including the reset that reprograms all of them; it is
what the IOKit subclass will call, and `docs/INTEGRATION.md` describes how that
is wired into the UniXNU-merlinx build.

M2 writes are behind `NATIVE_MALI_M2_WRITES`, the first M3 write (the GPU reset)
behind `NATIVE_MALI_M3_WRITES`, the M4 MMU enable/disable behind
`NATIVE_MALI_M4_WRITES` and the M6 job start behind `NATIVE_MALI_M6_WRITES`, all
default `0`. M5 adds no register of its own: a buffer's `map()` ends in the M4
range flush, so it fails closed while M4 writes are off. The host tests are the
only build that
defines them (see `docs/TESTING.md`); the driver refuses to touch a configuration
register until a human has read `probe()` back on the phone and accepted it
(roadmap rule 2).

`docs/OPEN_QUESTIONS.md` lists everything that needs a human answer before the
next milestone. `docs/TODO.md` lists code that is deliberately unimplemented.

## Layout

| Path | Role |
| --- | --- |
| `NativeMaliBoard.*` | device-tree parsing only (reg, the five named interrupts, clocks); no MMIO |
| `NativeMaliVm.*` | ARM_MALI_LPAE page tables and the AS_MEMATTR/AS_TRANSTAB values; publishes each table through an injected `NativeMaliCacheMaintainer` when the walker is not coherent; no MMIO |
| `NativeMaliMmu.*` | AS slot allocation, the `panfrost_mmu_enable()`/`disable()` sequence (LOCK/FLUSH/UPDATE, TRANSTAB/MEMATTR/TRANSCFG), range `FLUSH_PT` and the AS fault reads; writes behind `NATIVE_MALI_M4_WRITES` |
| `NativeMaliPlatform.h` | the single seam to hardware: read32/write32/delay, injected so host tests can drive real sequences |
| `NativeMaliPower.*` | M2 orchestration: read-only probe, fixed regulators, clock source/gate, the four MFG domains, polling, rollback and the reverse teardown (gate, then the domains in reverse) |
| `NativeMaliBringup.*` | M1→M4 in one place: read the platform, power up, identify/reset/power the GPU core, record `GPU_AS_PRESENT` and clear stale allocations, and undo it in reverse; owns no register knowledge, no job slots and no address space (an AS belongs to the client whose page tables it walks) |
| `NativeMaliPwrap.*` | the WACS2 transport to the PMIC (command word, FSM, timeouts, valid-bit clearing) |
| `NativeMaliMt6358.*` | the two GPU rails: VGPU/VSRAM selectors, enable bits, read-back and rollback |
| `NativeMaliHandleIndex.h` | shared handle → slot table (MIT, ported from Adreno740, formerly NativeAdreno) |
| `NativeMaliRegs.h` | generated from the pinned `panfrost_regs.h`; never edited by hand |
| `NativeMaliGpu.*` | GPU_ID → model, revision and Bifrost facts; reads no register |
| `NativeMaliGpuCore.*` | read-only GPU feature/present probe and `GPU_LATEST_FLUSH_ID`, `panfrost_gpu_soft_reset()`, the L2/shader/tiler power on/off and the GPU fault-register read; refuses to touch 0x13040000 unless `NativeMaliPower::gpuReadable()` agrees; writes behind `NATIVE_MALI_M3_WRITES` |
| `NativeMaliFault.*` | exception/access decode for the GPU and AS fault records from the pinned panfrost tables; no MMIO |
| `NativeMaliABI.h` | the fixed-size wire ABI of the future IOUserClient: fixed records, method numbers (including the device-wide `Reset`), `static_assert` layout; no IOKit and no register |
| `NativeMaliAbiValidate.*` | the checks one ABI record decides alone (sizes, alignment, ranges, permissions, fence shape) and the opaque fence encoding |
| `NativeMaliBoTable.*` | one client's BO state machine: monotonic handles, one address space, map/unmap through the real page tables, CPU tokens, unmap-then-release with retry; pure, no IOKit |
| `NativeMaliService.*` | the device as one object: bring-up plus the job slots, one address space per open client (allocate → prepare its tables → enable, and the reverse on close), the start/stop gates, and the reset (quiesce the job manager → soft reset → power on → mmu reset → clear the job event palette → drop the destroyed jobs → re-enable every client AS), which is also the target of the ABI Reset selector; pure, no IOKit |
| `NativeMaliUserClient.*` | the pure client above the BO table: Query/GetDeviceInfo, chain-head coverage, per-submission flush id, round-robin job-slot choice, own-fence bookkeeping, dependency waits, DONE/ERR/timeout reaping, fault/reset counters, an in-flight fence lost to a GPU reset reported as an error fence, fail-closed close and teardown, the Reset selector forwarded through an injected device-wide target, and the external-method dispatch (exact record sizes, untouched output on failure); no IOKit |
| `NativeMaliIrq.*` | decoding of the three interrupt status words (which GPU source fired, which job slot, which address space) from the pinned handlers; pure, so no handler and no interrupt claimed |
| `NativeMaliCache.h` | `clean`/`invalidate`/`clean+invalidate` as an injected interface (the XNU primitives are still an open question); no MMIO |
| `NativeMaliJob.*` | Bifrost v7 job header and WRITE_VALUE payload packing, 64-byte slots and the `Next` chain; caller-owned memory, no MMIO |
| `NativeMaliJobSlot.*` | `panfrost_job_hw_submit()` (`JS_HEAD_NEXT`/`JS_AFFINITY_NEXT`/`JS_CONFIG_NEXT`/`JS_FLUSH_ID_NEXT`, then `START`), the polled DONE/ERR wait per slot, one job at a time, per-slot software fences (`NativeMaliFence`, `fenceSignaled()`), and the reset pieces (`quiesce()` writes NOP/SOFT_STOP and polls `JOB_INT_JS_STATE`; `resetInterrupts()` writes `ALL_JS_INT_MASK`; `abandonOutstanding()` drops the destroyed jobs); writes behind `NATIVE_MALI_M6_WRITES` |
| `NativeMaliMemory.*` | buffer spans, `panfrost_mmu_map()`/`unmap()` through `NativeMaliVm` and `NativeMaliMmu::flushRange()`, and the non-coherent clean/invalidate policy; allocation and cache primitives are injected interfaces |
| `xnu/` | the IOKit glue as paper code only: window adapters, the `IOBufferMemoryDescriptor` page/buffer sources, the cache maintainer, `NativeMaliGpuService`/`NativeMaliGpuClient` and the `Info.plist`. **Never compiled against XNU headers or run**; every untested assumption is tagged `UNVERIFIED(XNU)`/`UNVERIFIED(HW)` and listed in `docs/UNVERIFIED.md` |
| `tools/generate-regs.sh` | regenerates `NativeMaliRegs.h`, refusing if the pin changed |
| `docs/TESTING.md` | how the logic is verified, what that proves, and how to add a test |
| `docs/UNVERIFIED.md` | what `UNVERIFIED(XNU)`/`UNVERIFIED(HW)` means, every place in `xnu/` that carries one, and what removing it requires |
| `tests/` | host tests, `XnuMemoryFixture.h` doubles, IOKit shim, the device DTB |
| `tests/run.sh` | one command: build and run every host test |
| `third_party/linux-panfrost/` | pinned Linux sources this driver is derived from |
| `third_party/mesa-panfrost/` | the Bifrost v7 descriptor layout (M6) |
| `third_party/device/` | this phone's own dumps: merged FDT, `/proc/iomem`, mali dmesg |
| `third_party/SOURCES.md` | provenance of everything under `third_party/`: origin, licence, committed or not |
| `third_party/PINNED.md` | provenance: what each code file is and its sha256 |
| `docs/DEVICE.md` | the facts measured on the phone, with the raw lines they come from |
| `docs/UPSTREAM.md` | which upstream supplies which facts, and how to refresh it |
| `docs/INTEGRATION.md` | how to wire this driver into UniXNU-merlinx: ADT node, submodule, `build-arm-driver.py`, `board.toml`, `Info.plist` |
| `AGENTS.md` | rules for agents working here: citations, write gate, tests, commits |

Planned module names, mirroring Adreno740 (its files are still named NativeAdreno*) one for one:

| NativeAdreno | NativeMali |
| --- | --- |
| Board | Board |
| Memory, Timer, KernelTime, Status | same names |
| Smmu*, Vm | Mmu, Vm (Mali address spaces and LPAE tables) |
| Ring, Submit | Job (job slots and fences) |
| Irq | Irq (GPU, MMU and JOB status decoding done; the handler waits on the XNU interrupt source) |
| Service, UserClient, Client, ABI | same names |
| Gmu*, CpInit, Pwrup*, Firmware*, Sha256 | none: the Job Manager path needs no firmware |

## Host tests

```
tests/run.sh          # build and run every host test
tests/run.sh --san    # same tests under AddressSanitizer + UBSan
```

Every test is a standalone binary built with the host compiler against
`tests/XnuMemoryFixture.h`; nothing here touches hardware. `tests/data/mt6768.dtb`
is this device's own device tree, used as the input of the board parser tests.
`docs/TESTING.md` explains what each assertion comes from and what the host tests
cannot prove.

## References and provenance

This driver follows **Adreno740** (formerly NativeAdreno; pinned at https://github.com/UniXNU/Adreno740/tree/5c8c1453c67d6ef6b10ec43ab1f8fc570a2f33e1) for its structure, error
handling and host-test style, and derives register, page-table and job knowledge
from the **Linux panfrost driver** and `io-pgtable-arm` (GPL-2.0-only, pinned in
`third_party/PINNED.md`). Facts about the board come from the device tree shipped
with this phone (`tests/data/mt6768.dtb`). No value in this repository is from
memory: every register offset carries the file and function it came from.

Because the work is derived from GPL-2.0 code, the whole driver is GPL-2.0-only.

## Licence

- The project as a whole is **GPL-2.0-only** (not "or later": the Linux files it
  derives from are GPL-2.0 without that option). See `LICENSE` and `NOTICE`.
- `NativeMaliRegs.h` is generated line by line from Linux `panfrost_regs.h`
  (GPL-2.0), and the driver follows panfrost's sequences, so it is a derived work.
- The one MIT file is `NativeMaliHandleIndex.h`, a literal port from Adreno740
  (formerly NativeAdreno); its original copyright notice is kept in the file and
  in `NOTICE`. MIT code may be relicensed into GPL-2.0-only, which is what the
  rest of the Adreno740-derived material is.
- Third-party reference copies under `third_party/` keep their own licences
  (Linux: GPL-2.0, Mesa: MIT); see `third_party/SOURCES.md` and `PINNED.md`.
- Please publish **source**, not prebuilt kernelcaches or boot images that
  bundle this driver with XNU and Apple components: GPL-2.0 and the Apple Public
  Source License do not combine cleanly. This is general information, not legal advice.
