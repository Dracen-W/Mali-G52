<!-- SPDX-License-Identifier: GPL-2.0-only -->
# 上游关系与同步方式

这套驱动不是把某一个上游项目 fork 过来用，而是从几处来源**取事实**，自己
写 XNU/IOKit 的实现。这份文档说明每个来源的地位、如何更新，以及哪些东西不能
跨来源搬。

## 1. 来源一览

| 来源 | 许可证 | 在本仓库的位置 | 用来做什么 | 是否入库 |
| --- | --- | --- | --- | --- |
| Linux panfrost + io-pgtable-arm | GPL-2.0 / GPL-2.0-only | `third_party/linux-panfrost/` | 寄存器偏移与位义、GPU 识别、页表格式、job slot 寄存器 | 是，原样保留 |
| Mesa panfrost (`v7.xml`, `pan_jc.h`) | MIT | `third_party/mesa-panfrost/` | Bifrost v7 的 job/描述符布局（M6） | 是，原样保留 |
| Adreno740（原 NativeAdreno） | MIT | `Adreno740-main/`、`NativeAdreno-reference-old/`（工作副本） | 模块结构、错误处理、user-client ABI 形状、host 测试风格 | 否 |
| MediaTek 厂商内核（MT6768） | GPL-2.0 / GPL-2.0-only | `third_party/mtk-android/`（从 `other reference/mtk-src.tar` 解压） | M2 的 SPM/时钟/PMIC 偏移、位、电压与顺序 | 否 |
| 本机设备树 | 板级数据 | `tests/data/mt6768.dtb`、`third_party/device/` | reg、中断、时钟、保留内存等**这块板子**的事实 | 是 |

`third_party/PINNED.md` 记录前两项每个代码文件的 sha256；
`docs/MTK_SOURCES.md` 记录 M2 直接引用的厂商文件的 sha256；
`tests/data/SHA256SUMS` 记录设备树的哈希。没有哈希的引用不算钉死。

## 2. panfrost：事实来源，不是代码来源

- `NativeMaliRegs.h` 由 `tools/generate-regs.sh` 从钉死的
  `panfrost_regs.h` 逐行生成。脚本先校验源文件哈希，不一致就拒绝生成；
  `tests/run.sh` 每次重新生成并 `cmp`，所以「生成物是旧的」会在测试前失败。
- 驱动不 include Linux 头、不调用 Linux API。寄存器名与位义是人类可读的
  对照表，具体访问走 `NativeMaliRegisterTarget`。
- panfrost 的某些常量假设了别的 SoC（例如默认的 `ias`/`oas`）。本驱动不用
  默认值：等 M3 从 `GPU_MMU_FEATURES` 读回真实位宽，读不到就拒绝构造 MMU。

## 3. Adreno740（原 NativeAdreno）：结构参照，MIT

- 固定在 https://github.com/UniXNU/Adreno740/tree/5c8c1453c67d6ef6b10ec43ab1f8fc570a2f33e1。仓库已改名，其中的源文件仍使用 `NativeAdreno` 前缀。

- 参照的是形状：`Board`/`Vm`/`Power` 的职责边界、`IOReturn` 错误传播、
  host 测试的组织方式。
- 唯一按文件移植的是 `NativeMaliHandleIndex.h`（来自
  `NativeAdrenoHandleIndex.h`）。它保持 MIT 头；其余新文件是 GPL-2.0-only。
- 同步方式：拿新版 `NativeAdrenoHandleIndex.h` 做 diff，只接受与句柄表语义
  有关的改动；如果上游顺手改了命名风格，只同步语义，不跟着改本仓库的命名。

## 4. Mesa：M6 的描述符规范

- `v7.xml` 与 `pan_jc.h` 是 Bifrost v7 的布局规范。M6 写 job chain 时逐字段
  对照，并在注释里写 XML 的路径。
- Mesa 是 MIT，但**不要把 Mesa 的 C 代码复制进驱动**；只取结构定义，按本仓库
  的风格重写，并在头文件注明「布局来自 Mesa 的哪个文件」。

## 5. MediaTek 厂商内核：M2 的事实来源

这批文件不入库（体积大、且是手机厂商的整包源码），所以：

- 每个被引用的文件在 `docs/MTK_SOURCES.md` 里有 sha256；行号漂移时先比哈希，
  不一致就重新核对引用。
- 只取**寄存器、位、顺序、电压**，不复制厂商的 C 代码结构。厂商代码里大量
  依赖 Linux clock/regulator 框架，而本驱动是直接把序列写清楚。
- 厂商归档里缺 MT6768 自己的 gpufreq 平台文件时，用同族文件的顺序并明确标注
  「同族来源」，不假装它是 MT6768 的文件。M2 的 VSRAM-before-VGPU 就是这种
  情况（`mt6785/mtk_gpufreq_core.c`，见 `NativeMaliMt6358.h`）。

## 6. 更新一个来源时的步骤

1. 替换参考副本（不要动已入库的 `third_party/linux-panfrost/` 和
   `third_party/mesa-panfrost/`，除非是有意的版本升级）。
2. 更新对应的哈希文件（`third_party/PINNED.md`、`docs/MTK_SOURCES.md`、
   `tests/data/SHA256SUMS`）。
3. 重新生成派生文件（`tools/generate-regs.sh`）。
4. 跑 `tests/run.sh` 与 `tests/run.sh --san`；失败先看是哈希、生成物还是引用
   行号漂移。
5. 逐条复核受影响的注释：路径、行号、函数名、位的含义。
6. 在提交信息里写清「哪个来源、哪次更新、哪些引用随之改变」。

## 7. 不能做的事

- 不能因为「另一个 SoC 上这样写」就把地址/位搬过来；MT6768 的值只认 MT6768
  的文件和这块板子的 DTB。
- 不能把厂商代码里的 Linux 框架调用当成序列的一部分（例如假设
  `clk_prepare_enable` 会替我们做门控）；序列必须自己写全。
- 不能把参考副本提交进仓库，也不能把 `.tar` 放进去。
