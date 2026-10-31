# 第三方材料的来源

这份文件说明 `third_party/` 里**每个东西从哪来、什么许可、入不入库、被谁引用**。
按路线图第 1 节守则 1：驱动里的每个偏移、位定义和魔数都要能追到这里的某一行。
逐文件的 sha256 在 [PINNED.md](PINNED.md)，设备实测数据在 [../docs/DEVICE.md](../docs/DEVICE.md)。

| 路径 | 内容 | 来源 | 许可 | 入库 |
| --- | --- | --- | --- | --- |
| `linux-panfrost/` | 15 个上游文件：`drivers/gpu/drm/panfrost/*`、`drivers/iommu/io-pgtable-arm.c`、`include/uapi/drm/panfrost_drm.h`、`Documentation/devicetree/bindings/gpu/arm,mali-bifrost.yaml` | 项目所有者 2026-10-24 从 Linux 主线的源码直接取（部分因 429 手工复制）；**不对应任何 tag** | GPL-2.0 / GPL-2.0-only（逐文件 SPDX） | 入库，逐文件哈希见 `PINNED.md` |
| `mesa-panfrost/v7.xml` | Bifrost v7 描述符位布局 | 项目所有者 2026-10-25 提供，原路径 `src/panfrost/genxml/v7.xml` | MIT（Collabora 等） | 入库，哈希见 `PINNED.md` |
| `mesa-panfrost/pan_jc.h` | job chain 构造与 header 字段 | 同上，原路径 `src/panfrost/lib/pan_jc.h` | MIT | 入库，哈希见 `PINNED.md` |
| `mtk-android/` | 2635 个文件的厂商内核子集（SPM/MT6768、gpufreq、clk-mt6768、pmic/mt6358、pwrap、dts 等） | 项目所有者 2026-10-24 提供的 `other reference/mtk-src.tar`（56 MiB） | MediaTek + GPL-2.0 混合 | **不入库**（`.gitignore`）；只作参考，不参与编译 |
| `device/merlin.fdt` | 这台手机合并后的完整 FDT（含 dtbo overlay），252 KB | 所有者从设备上 dump（`other reference/others/merlin.fdt`） | 板级数据，非代码 | 入库 |
| `device/iomem.txt` | 设备上的 `/proc/iomem` | 同上 | 设备输出 | 入库 |
| `device/mali_dmesg.txt` | 设备上厂商 mali 驱动的 probe 日志（11 行） | 同上（`other reference/grab_out/mali_dmesg.txt`） | 设备输出 | 入库 |
| `PINNED.md` | 代码类来源的哈希与「哪些还没拿到」 | 本仓库 | GPL-2.0-only | 入库 |
| `SOURCES.md` | 本文件 | 本仓库 | GPL-2.0-only | 入库 |

## 各材料解决什么

| 材料 | 用在哪 |
| --- | --- |
| `linux-panfrost/io-pgtable-arm.c` | `NativeMaliVm` 的 `ARM_MALI_LPAE` 几何、描述符编码、TRANSTAB/MEMATTR 值 |
| `linux-panfrost/panfrost_regs.h` | `NativeMaliRegs.h` 由它生成（`tools/generate-regs.sh`，哈希不符即拒绝生成） |
| `linux-panfrost/panfrost_gpu.c` / `panfrost_device.h` | `NativeMaliGpu::identify()` 的型号表、产品号掩码、修订号解码 |
| `linux-panfrost/panfrost_mmu.c` | 格式选择（Mali LPAE vs AArch64 4K）、`AS_TRANSCFG` 与 `AS_COMMAND` 序列 |
| `mesa-panfrost/v7.xml` | M6 的 WRITE_VALUE job 布局（payload 在 +32，Address/Type/Immediate 在 +32/+40/+48） |
| `mesa-panfrost/pan_jc.h` | M6 的链式规则：`next`、`dependency_1/2` 索引、`write_value_index` |
| `mtk-android/.../spm/mt6768/` | `NativeMaliPower` 的 SPM 偏移、`PWR_CON` 位名、`PWR_STATUS` 的 MFG 位 |
| `mtk-android/drivers/clk/mediatek/clk-mt6768*.c` | M2 的 PLL（apmixed `0x248..0x254`）、`MFG_CG_CON/SET/CLR`（mfgcfg `0x000/0x004/0x008`）、MFG 四个域的 `sys_ops` 与保护协议 |
| `mtk-android/.../mt6768/.../upmu_hw.h` + `pmic/mt6358/` | M2 电压步骤的 BUCK_VGPU / LDO_VSRAM_GPU 寄存器与电压选择器 |
| `device/*` | GPU 型号（`0x70021000` → g52 r1p0）、窗口大小、pwrap 基址；见 `docs/DEVICE.md` |

## 相关但不在 third_party 的东西

| 路径 | 说明 |
| --- | --- |
| `NativeAdreno-reference/` | Adreno740（原 NativeAdreno，MIT，Copyright NativeAdreno contributors，固定在 5c8c1453c67d6ef6b10ec43ab1f8fc570a2f33e1）的参考副本，学习结构/接口/测试风格。已 gitignore，不入库，不参与编译。 |
| `tests/data/mt6768.dtb` | 这台手机的设备树，**解析器测试的输入**。板级数据，入库，哈希在 `tests/data/SHA256SUMS`。它比 `device/merlin.fdt` 小，因为我们只把解析需要的部分带进了仓库。 |
| `other reference/` | 所有原始素材（tar、img、dump、mesa 原件）。已 gitignore；`third_party/` 里的副本才是被引用的那份。 |

## 还缺什么（拿不到就不猜）

| 缺的 | 挡住谁 | 说明 |
| --- | --- | --- |
| `src/panfrost/genxml/common.xml` | M6 的 job header 位段 | `v7.xml` 只写了 `type="Job Header"`，字段位定义在这个文件里 |
| 完整的 `drivers/misc/mediatek/`（若手上有） | M2 的完整性 | 现有子集够用；gpu/mfgsys 与 mt6768 的 mtcmos 模块不在其中 |
| `GPU_MMU_FEATURES` 的 VA/PA 位宽 | M4 的 `ias`/`oas` 取值 | 只能上机读（M3） |
