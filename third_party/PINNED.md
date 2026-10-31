# Pinned sources

Rule 1 of the roadmap: every register offset, bit definition and magic number in
this driver must cite a pinned source. This file is that pin. A value whose
source is `unverified` may not be used in code.

## Linux (panfrost, io-pgtable-arm)

- Source: `torvalds/linux` at commit `9944dd07f3cf7b6c19d65c2585147887ca06677c`
  (2026-10-12, "drm/panfrost: Fix races between perfcnt and reset sequence"),
  https://github.com/torvalds/linux/tree/9944dd07f3cf7b6c19d65c2585147887ca06677c
- Pin model: **commit pin plus content pin**. Every file below is byte-identical
  to that commit (verified on 2026-10-31 by downloading each path from
  raw.githubusercontent.com at the commit and comparing sha256). Code cites
  `path:function` plus the hash of that path, so a vendored file cannot change
  under a citation.
- History: the batch first collected on 2026-10-24 was taken from GitHub without
  recording the commit. Nine files lacked the final newline and the
  `arm,mali-bifrost.yaml` binding came from a later `master` (it contained
  `samsung,exynos850-mali`, which the pinned commit does not have). Those ten
  files were replaced by the exact files from the pinned commit; `panfrost_regs.h`,
  `panfrost_gpu.[ch]`, `panfrost_issues.h` and `panfrost_job.c` were already
  identical, so `NativeMaliRegs.h` is unaffected.

Files, byte-identical to the pinned commit, with sha256. Paths keep the upstream layout.

| sha256 | path |
| --- | --- |
| `7a1ca8382e2b7c636b45460ea92584d9912fb742141df433bb65ff3d3ab856e3` | `Documentation/devicetree/bindings/gpu/arm,mali-bifrost.yaml` |
| `14a10f647750de7025a54235ffc7e0a591694835582258cc13812d1901180ddd` | `drivers/gpu/drm/panfrost/panfrost_device.c` |
| `af6de931adbc4ab5c97cb11dd8d8a1cefe070251817e46a46583e0a765fc59ad` | `drivers/gpu/drm/panfrost/panfrost_device.h` |
| `9397a3efd8db4dc04e3d104c8b61bf439199edc2715ef3e0040e898646467cfc` | `drivers/gpu/drm/panfrost/panfrost_drv.c` |
| `126df115a5a71e2ebb486fa766a83bc1d9ad2ea9216e01f293748dc577fb84ce` | `drivers/gpu/drm/panfrost/panfrost_features.h` |
| `f3da3ed2851320f4464c1f2e40a570d41b94095daf223a050e5f3aeb09ea984d` | `drivers/gpu/drm/panfrost/panfrost_gpu.c` |
| `2434c816a4bf0cc10dabd8bb43e6e44e86221aeae83eb97af042b85efd226402` | `drivers/gpu/drm/panfrost/panfrost_gpu.h` |
| `baed6104faf634731ec556d1b32542adbe1fb2579fcb5e9a9ae87badddf333b1` | `drivers/gpu/drm/panfrost/panfrost_issues.h` |
| `dd7e538990eea41112b1304652ecffcd4dce3940f35805fc793e215ad45f4688` | `drivers/gpu/drm/panfrost/panfrost_job.c` |
| `545be951bb75d528321d0c637a2ea00c6c4c58f5835cf1a2879f76ccb060b0f7` | `drivers/gpu/drm/panfrost/panfrost_job.h` |
| `99d808492218cb5bfe2e681922885d245d35ca9f9a36cee466aa7923cebe89d5` | `drivers/gpu/drm/panfrost/panfrost_mmu.c` |
| `c0ad04975212db362354daf3b852b6ce682700bbcecd461c5ab69c255f14083f` | `drivers/gpu/drm/panfrost/panfrost_mmu.h` |
| `a11d09f281dd79976b35273627be3ff1857911506e2637510b9a9098bc6ceb18` | `drivers/gpu/drm/panfrost/panfrost_regs.h` |
| `41885615ee42cadf527eda2be1066f9dbf8ee60e3d6f1d96f11a639886d4255e` | `drivers/iommu/io-pgtable-arm.c` |
| `c6e8060c586e5fca48a3ea68ad6e90f393cbef32d347cf64becd744f46f19169` | `include/uapi/drm/panfrost_drm.h` |

All files were checked to be real sources (SPDX header, plausible size), not the
313-byte GitHub rate-limit page.

Derived, generated output: `NativeMaliRegs.h` is produced from
`drivers/gpu/drm/panfrost/panfrost_regs.h` by `tools/generate-regs.sh`, which
refuses to run when that file's sha256 no longer matches the value above.
`tests/run.sh` regenerates it and compares, so the committed copy cannot drift
from this pin, and `tests/test_regs.cpp` locks the offsets the driver uses.

### Missing from the batch

| path | needed by |
| --- | --- |
| `include/linux/io-pgtable.h` | M4 (page-table operations and quirks) |

## Adreno740 (formerly NativeAdreno)

- Source: `UniXNU/Adreno740` at commit `5c8c1453c67d6ef6b10ec43ab1f8fc570a2f33e1`,
  https://github.com/UniXNU/Adreno740/tree/5c8c1453c67d6ef6b10ec43ab1f8fc570a2f33e1
  The repository was renamed from NativeAdreno; its source files still carry the
  `NativeAdreno` prefix. Licence: MIT, `Copyright (c) 2026 NativeAdreno contributors`.
- Only one file is ported verbatim: `NativeAdrenoHandleIndex.h`
  (sha256 `12a0627f00b8131452806cf81639ddd69c54d1b79367c39cee2bc48158afd202`) became `NativeMaliHandleIndex.h`. After renaming
  `NativeAdreno` to `NativeMali`, the two differ only in the header comment and
  the include guard. `LICENSE` at that commit has sha256 `16fbc0170994a48f60938d9f0d992d3bad113383cf90882f75c206e079c2a2dc`.
- Everything else under `xnu/` and the module layout follows its structure; those
  files are GPL-2.0-only (see `NOTICE`).

## Mesa (needed by M6, optional for M9)

Two files, supplied by the project owner on 2026-10-25 and copied verbatim into
`third_party/mesa-panfrost/`. Both are MIT (Collabora / Alyssa Rosenzweig /
Broadcom), unlike the Linux batch, so nothing here changes this project's
GPL-2.0-only terms; they are read as specifications, not linked.

| sha256 | path | what it provides |
| --- | --- | --- |
| `0d86661a755b749fd49ac3645e10ea8222a3618bb2b25167be8bb91ef5507f36` | `src/panfrost/genxml/v7.xml` | the Bifrost v7 descriptor bit layout |
| `aabd45b5128e29b0040dd0b5a35f81a03c7a94f670a2ee0dfcf3161f3008fbe0` | `src/panfrost/lib/pan_jc.h` | the job-chain builder and the header fields it fills |

What v7.xml already pins for M6's first proof, quoted from it:

```
<struct name="Write Value Job Payload">
  <field name="Address" size="64" start="0:0" type="address"/>
  <field name="Type" size="32" start="2:0" type="Write Value Type"/>
  <field name="Immediate Value" size="64" start="4:0" type="uint"/>
</struct>
<aggregate name="Write Value Job" align="64">
  <section name="Header" offset="0" type="Job Header"/>
  <section name="Payload" offset="32" type="Write Value Job Payload"/>
</aggregate>
```

So a WRITE_VALUE job is 64-byte aligned: header at +0, payload at +32, address
at +32, type word at +40, immediate at +48, and the type enum is 0..7 with
`Immediate 32 = 6` and `Immediate 64 = 7`.

### Still missing from the batch

| path | needed by |
| --- | --- |
| `src/panfrost/genxml/common.xml` | the `Job Header` type, which v7.xml only names. Without it the header fields (type, barrier, index, dependency_1/2, next, payload size) have no pinned bit positions, so the WRITE_VALUE job chain stays a marked TODO. |

Not needed: `gen_pack.py`, `gen_macros.h`, `cs_builder.h` (build-time packing
help), and `src/gallium/drivers/panfrost/pan_job.*` (userspace submission).

## MT6768 vendor kernel (needed by M2)

Received 2026-10-24 as `other reference/mtk-src.tar` (56 MiB, 2635 files), then
extracted to `third_party/mtk-android/` and **not committed** (gitignored).
Present and checked:

| path | role |
| --- | --- |
| `drivers/misc/mediatek/base/power/spm/mt6768/mtk_spm_reg.h`, `pwr_ctrl.h`, `mtk_spm_power.c` | SPM state and MFG power bits (`PW_MFG_REQ_MASK_B`) |
| `drivers/misc/mediatek/base/power/mt6768/mtk_gpufreq_core.c`, `mtk_gpufreq.h` | GPU DVFS entry, uses the scpsys/topckgen clocks above |
| `drivers/misc/mediatek/pmic/mt6358/v1/` + `pmic/include/mt6358/` | MT6358 buck/ldo API (vgpu, vsram_gpu) |
| `drivers/misc/mediatek/pmic_wrap/mt6768/pwrap_hal.c` | PMIC transport |
| `drivers/clk/mediatek/clk-mt6768.c`, `clk-mt6768-pg.c`, `mt6768_clkmgr.c` | topckgen/mfg_cfg clock gates |
| `arch/arm64/boot/dts/mediatek/mt6768.dts` | board DTS |

Still missing for M2: an MT6768 `gpufreq` platform file (only the generic
`mtk_gpufreq_core` is present) and any `mfgsys`/mfg power helper. The vendor mali
driver in the archive is for mt6873 (Valhall), not this phone's Bifrost, so it is
not a register reference for us.

## Device tree

`tests/data/mt6768.dtb` is the phone's own device tree as supplied by the owner.
It is board data, not code; the board parser tests read it directly, so every DT
fact in the driver is checked against real bytes. Its sha256 is in
`tests/data/SHA256SUMS` (generated with `shasum -a 256`, not typed):

```
1fa1636b04133efc95ac028665d2809c6fe104a67f66d48fb7e46e25d3ac6a4e  mt6768.dtb
```
