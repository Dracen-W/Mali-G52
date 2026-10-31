<!-- SPDX-License-Identifier: GPL-2.0-only -->
# 这台机器的实测事实

只记**从设备上直接读到的**东西，不记推测。每条都带原始行和文件哈希，方便复核。
被引用的三个 dump 已经收进 [third_party/device/](../third_party/device/)（来源说明见
[third_party/SOURCES.md](../third_party/SOURCES.md)）；`other reference/` 里那份原件已 gitignore。

| 文件 | sha256 | 来源 |
| --- | --- | --- |
| [`third_party/device/mali_dmesg.txt`](../third_party/device/mali_dmesg.txt) | `aceeee0469e4b242ce7cb930f0c60ba1ae1c3175c9304b60cc38dc225cd8f3a4` | `dmesg` 过滤出的 mali 行 |
| [`third_party/device/iomem.txt`](../third_party/device/iomem.txt) | `fc6511cdc6b5274b253599f3f8d07aae1ffd647f45fdc6c4bd908a057bad1738` | `/proc/iomem` |
| [`third_party/device/merlin.fdt`](../third_party/device/merlin.fdt) | `c08a8938c446efd034c3fefaafafa738f73454af90f20b7dc06a2db44cbe729b` | 合并后的完整 FDT |
| `other reference/others/dmesg-hw.txt`（未入库） | `d01463dbeaa975019e2fe3fefb7e7c610ef868d1b1c421e2ef3e2d1d12c66418` | 早一次开机的 dmesg 片段（352 行，无 mali 部分） |

## 1. 身份

`grab_out/key_findings.txt`：

```
Redmi/merlin_global/merlin:12/SP1A.210812.016/V13.0.2.0.SJOMIXM:user/release-keys
4.19.325-cip136-droidspaces-st20-g4798e3c7517e
```

即 Redmi（merlin 代号，MT6768）、Android 12、厂商 4.19 内核（droidspaces 变体）。
设备已 root（Magisk），装了 `mali_r49p1_mt6768_slim` 模块，用 r49p1 的用户态 blob
替换 `/vendor/lib{,64}/egl/libGLES_mali.so` 与 `hw/vulkan.mali.so`（模块副本与
vendor 副本 sha256 一致，替换生效）。

## 2. GPU：product 0x7002（g52），revision r1p0

`grab_out/mali_dmesg.txt` 原文：

```
[    1.365543] mali 13040000.mali: Kernel DDK version r49p1-04eac0
[    1.367398] mali 13040000.mali: Register LUT 00070200 initialized for GPU arch 0x00070400
[    1.368756] mali 13040000.mali: GPU identified as 0x2 arch 7.4.0 r1p0 status 0
[    1.370608] mali 13040000.mali: No OPPs found in device tree! Scaling timeouts using 100000 kHz
```

按 panfrost 的拆法（`NativeMaliGpu.h` 里记的 `panfrost_gpu.c` 规则）：

- `LUT 00070200` → 架构 7、product `0x02`，即 **product id `0x7002`**，正是
  `gpu_models[]` 里的 `g52`。所以 `GPU_ID` 应是 **`0x70021000`**：
  product `0x7002`，revision `0x1000` → major 1、minor 0、status 0，与 `r1p0 status 0` 一致。
- `tests/test_gpu.cpp` 把这个值作为真机黄金用例。
- 这台机器是 **Bifrost v7（不是 Valhall）**，也印证了包里那份 mt6873 的 Valhall 驱动
  不能当寄存器参考的结论。

## 3. 窗口与小节点（`/proc/iomem` + 两棵设备树）

```
00110b80-00110bff : dvfsrc@10012000
10001000-10001fff : scp_infra@10001000
10007000-10007fff : toprgu@10007000
1000d000-1000dfff : pwrap
10012000-10012fff : dvfsrc@10012000
13040000-13043fff : 13040000.mali
```

- **Mali 窗口 = `0x13040000`、`0x4000`**：和设备树、`NativeMaliBoard` 的解析结果完全一致。
- **pwrap = `0x1000d000`、`0x1000`**：M2 电压步骤要经过它，现在有基址和大小了。
- `scp_infra@10001000` 就是设备树里的 `/scpsys@10001000`（同基址），1 页。
- `toprgu@10007000`（复位控制器）也在，M3 软复位卡住时可以拿它做兜底，但先不动。
- mfg_cfg 与 topckgen 的窗口大小**由设备树给出**（两个树一致）：
  `/mfg_cfg@13000000` `reg` = 基址 `0x13000000`、大小 `0x1000`；
  `/topckgen@10000000` 同样是 `0x1000`。`/scpsys@10001000` 是 syscon（`reg` 有
  144 字节，多个区段），我们只按需取。
- **SPM（`0x10006000`）、mfg_cfg、topckgen 不在 iomem 里**：厂商驱动没有对它们
  `request_mem_region`。SPM 的基址与窗口大小现在由设备树确认：`/scpsys@10001000`
  的 `reg` 第二段就是 `<0 0x10006000 0 0x1000>`（注释为 `spm`），所以映射 **1 页**
  是设备树给出的事实，不再是「按所需寄存器估算」。我们只用 `0x180`/`0x184` 和
  `PWR_CON` 的 `0x310..0x34c`，都在这一页内。
- 不要和 `SPM-reserved` 搞混：那是 `/reserved-memory/mblock-16-SPM-reserved`，
  地址 `0x77ff0000`、大小 `0x10000`、`no-map`，是 SPM 固件/挂起用的保留内存，
  **不是** SPM 的寄存器块。

## 4. 没有 GPU DVFS

`No OPPs found in device tree!`：DDK 在设备树里找不到 GPU 的 OPP 表，改用固定
100 MHz 做超时换算。MTK 的 gpufreq 是另一套（`/proc/gpufreq`），不通过 DT OPP。

结论：M2 第一版**固定电压 + 固定时钟**与这台机器的现状一致，不做 DVFS 不会丢功能。
（`tests/data/mt6768.dtb` 里确实有 `opp_table0`/`opp0..8`，但那是 CPU 的。）

## 5. 两棵设备树的关系（`merlin.fdt` vs `tests/data/mt6768.dtb`）

用同一个解析器把两棵树的全部「节点 + 属性 + 值」列出来做差集：

- `/mali@13040000`、`/gpufreq`、`/scpsys@10001000`、`/mfg_cfg@13000000` 这四个节点的
  属性**完全相同**（数量和值都一样）。所以 `merlin.fdt` 没有提供任何新的 GPU 事实。
- `merlin.fdt` 多出来的那些行全是别的板级节点（`/accdet`、`/charger`、`/board_id`、
  `/bootprof`、`/reserved-memory/*` …），与 GPU 无关。
- 两棵树里 `/mali@13040000` 都带 `ged-supply = <0x5f>`（MTK 的 GED，做负载/boost
  调度）。我们不需要它；dmesg 里那句 `Linked as a consumer to ged` 说的就是它。
  设备树的 mali 节点**没有 `clocks`**——八个时钟全在 `/gpufreq` 下，驱动不能
  假设它们在 mali 节点上（`NativeMaliBoard` 也是分开解析的）。
- 结论：解析器测试继续用 `tests/data/mt6768.dtb`（小、够用），`merlin.fdt` 作为完整
  板级档案留在 `third_party/device/`。

## 6. 仍然未知（别在这份文档里编）

| 事实 | 怎么才能知道 |
| --- | --- |
| MMU VA/PA 位宽（`ias`/`oas`） | M3 读 `GPU_MMU_FEATURES`（0x014）的 [7:0] 和 [15:8]；kbase 这份日志没打印 |
| GIC 上这五条线是否真的电平低 | 现在只有设备树（0x8）与我们的人工确认；`/proc/interrupts` 只证明它们被注册了 |
| `buck_vgpu` 的实际初始电压 | `/sys/kernel/debug/regulator/regulator_summary`（还没拿到）；M2 的 `probe()` 也会经 pwrap 读回 |
