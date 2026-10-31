<!-- SPDX-License-Identifier: GPL-2.0-only -->
# 厂商内核源码清单（MT6768）

## 已收到（2026-10-24）

`other reference/mtk-src.tar`（56 MiB、2635 个文件）已解压到
`third_party/mtk-android/`，该目录 gitignore，**不入库**。核对结果：

- 有：`base/power/spm/mt6768/`（含 `pwr_ctrl.h` 的 `PW_MFG_REQ_MASK_B`、
  `mtk_spm_reg.h`、`mtk_spm_power.c`）、`base/power/mt6768/mtk_gpufreq_core.c`、
  `pmic/mt6358/v1/`、`pmic_wrap/mt6768/pwrap_hal.c`、
  `clk/mediatek/clk-mt6768*.c`、`arch/arm64/boot/dts/mediatek/mt6768.dts`。
- 缺：MT6768 自己的 gpufreq 平台文件（只有通用 `mtk_gpufreq_core`），
  以及 mfgsys / mfg 电源封装。M2 的应对：
  - SPM 的 `PWR_CON`/`PWR_STATUS` 偏移与位义直接取 `clk-mt6768-pg.c`
    （`spm_mtcmos_ctrl_mfg*`），不再依赖缺失的 mfgsys 封装；
  - 两路 PMIC 的使能先后顺序取兄弟平台
    `base/power/mt6785/mtk_gpufreq_core.c:429-462`（VSRAM 先于 VGPU），
    这一点在 `NativeMaliMt6358.h` 里明确标注为「同族文件的顺序」；
  - 电压值取 `base/power/mt6768/mtk_gpufreq_core.h` 自己 OPP 表的最低档。
- 注意：归档里的 mali 驱动是 mt6873（Valhall），不是这台手机的 Bifrost，
  不能当寄存器参考。

## M2 直接引用的文件（内容钉死）

`third_party/mtk-android/` 不入库，所以引用不能只写路径。下表是 M2 注释里
点到名的每个文件在**本机归档**中的 sha256；以后重新取源码时先比这张表，
不一致就说明行号可能已经漂移，代码里的引用要重新核对。

| 文件 | sha256 |
| --- | --- |
| `drivers/clk/mediatek/clk-mt6768-pg.c` | `d8fcee5f2aebafcf82395fdd2a573afa8d1d020acb50683881a108dbdabddad6` |
| `drivers/clk/mediatek/clk-mt6768.c` | `45bd68b1d9935cfeddab7707bd00c1a9df20c0318e02457d2f1b33fb2a7097c9` |
| `drivers/regulator/mt6358-regulator.c` | `b1f36fd3e4243d8944db135e647559a7ffda4e005cf18e3fc0e559722c13a6b0` |
| `drivers/misc/mediatek/pmic_wrap/mt6768/pwrap_hal.c` | `29df599d2193e724e394a4a9dffe8ac0b6ffbf78fc92e84352b497e95486b3bc` |
| `drivers/misc/mediatek/pmic_wrap/mt6768/pwrap_hal.h` | `5b9dbce5ffcf78d62432a47a74e4c7934a34d6b392c0d199c06fc7cf88451dea` |
| `drivers/misc/mediatek/include/mt-plat/mt6768/include/mach/upmu_hw.h` | `dd3b3eec9c11a708457c659e5001cde10ce4cf97f7573550a99a0a9f9f31f19c` |
| `drivers/misc/mediatek/base/power/mt6768/mtk_gpufreq_core.h` | `04056f9a04e8da0c24d85ca6537a7efc60da2bfd64c4c49e2121de45f16fa61b` |
| `drivers/misc/mediatek/base/power/mt6785/mtk_gpufreq_core.c` | `03741a39e8c05a30a2145614dbbaeb091cece631f5c76db35efebe764de5de11` |
| `arch/arm64/boot/dts/mediatek/mt6768.dts` | `6096684d30b66b0b7b71c06a9f2bd7244b35172bc0c53820dfcca24bd41c94c0` |

`NativeMaliPower.h`、`NativeMaliMt6358.h`、`NativeMaliPwrap.h` 的注释按
「路径 + 行号/函数」引用这些文件。真机 DTB 的哈希在
`tests/data/SHA256SUMS`，与上表的 `.dts` 是同一棵树的两种形式（编译前后）。


下面是当时给你的收集说明，留作补充取用。

把你那台机器的 Android kernel 里下面这些取出来，**保持相对路径**放到
`third_party/mtk-android/`（这个目录默认不入库，见 `.gitignore`），然后告诉我
内核版本和厂商分支名，我写进 `third_party/PINNED.md`。

## 一次性收集（最省事）

在 kernel 根目录执行：

```sh
mkdir -p /tmp/mtk
git ls-files \
  'drivers/misc/mediatek/gpu/**' \
  'drivers/misc/mediatek/base/power/**' \
  'drivers/misc/mediatek/pmic/**' \
  'drivers/misc/mediatek/pmic_wrap/**' \
  'drivers/misc/mediatek/include/mt-plat/mt6768/**' \
  'drivers/misc/mediatek/include/mt-plat/*.h' \
  'drivers/regulator/**' \
  'drivers/clk/mediatek/**' \
  'drivers/soc/mediatek/**' \
  'drivers/gpu/drm/mediatek/**' \
  'include/dt-bindings/clock/mt6768*' \
  'include/dt-bindings/power/mt6768*' \
  'include/linux/regulator/**' \
  'arch/arm64/boot/dts/mediatek/mt6768*' \
  'arch/arm64/boot/dts/mediatek/*.dts' \
  | tar -cf /tmp/mtk-src.tar -T -
```

体积会比较大；如果只想先解 M2，按下面「最小集合」取即可。

## 最小集合（M2 电源/时钟/供电）

| 用途 | 典型路径（版本不同会变，用 `git grep` 找） |
| --- | --- |
| GPU 频率与电源入口 | `drivers/misc/mediatek/gpu/gpufreq/`（`gpufreq.c`、`gpufreq_mt6768.c`、`gpufreq_mt6768.h`、`gpufreq_mt6768_plat.c`） |
| GPU 电源域封装 | `drivers/misc/mediatek/gpu/mfgsys/`（`mfgsys.c`、`mt6768/…`） |
| 电源域（mtcmos） | `drivers/misc/mediatek/base/power/mt6768/mtcmos.c` 或 `.../power/mtcmos/…` |
| SPM（睡眠/上电状态） | `drivers/misc/mediatek/base/power/mt6768/spm_mt6768.c`、`.../base/power/spm/…`、`.../include/spm_v2/spm.h`、`spm_mtcmos.h` |
| MT6358 稳压器 | `drivers/misc/mediatek/pmic/mt6358/`（`mt6358-regulator.c` 或 `regulator/` 子目录）、`.../pmic/include/mt6358/` |
| pwrap | `drivers/misc/mediatek/pmic_wrap/mt6768/`（`pwrap.c`、`pwrap_mpu.c`） |
| topckgen / mfgcfg 时钟 | `drivers/clk/mediatek/clk-mt6768.c`、`clk-mt6768-pg.c`、`include/dt-bindings/clock/mt6768-clk.h` |
| 本机 DTS（含真实时序） | `arch/arm64/boot/dts/mediatek/mt6768.dtsi` 与这台手机的板级 dts |

用 `git grep -l "mt6768" -- drivers/misc/mediatek drivers/clk drivers/regulator drivers/soc`
可以确认你那个版本里的确切路径。

## 最小集合（M3/M6 校验用，可选）

| 用途 | 路径 |
| --- | --- |
| 厂商 mali DDK（Bifrost v7，用来核对寄存器/上电顺序） | `drivers/misc/mediatek/gpu/gpu_mali/…`（`mali_kbase_*.c`） |
| GED / GPU 调度 | `drivers/misc/mediatek/gpu/ged/` |
| 显示（M8 之后才用） | `drivers/misc/mediatek/video/`、`drivers/gpu/drm/mediatek/` |

## 我拿到之后会做什么

1. 只读：把 `mtcmos_mfg*`、`subsys_mfg_cg`、`clk_mux/main/sub` 的寄存器偏移与
   位定义摘出来，每一条在注释里写「文件 + 函数」。**已完成**，见
   `NativeMaliPower.h` 第 1–6 节、`NativeMaliPwrap.h`、`NativeMaliMt6358.h`。
2. 写一个**只读**的探测模块（编译开关默认关闭），读 SPM 的 MFG 上电状态。
   **已完成**：`NativeMaliPower::probe()`，本身不配置任何寄存器；它只经 pwrap
   读 PMIC，而 pwrap 读必须写一次 VLDCLR，这一点在头文件里写明。
3. 你在串口上确认状态后，我才写第一版上电序列（固定保守电压，失败按相反顺序回滚）。
   序列已写好并只在 host 假寄存器上跑过；真实写入由 `NATIVE_MALI_M2_WRITES`
   控制，默认 0，驱动构建不定义它。**在你看过真机 `probe()` 输出之前不要打开。**
