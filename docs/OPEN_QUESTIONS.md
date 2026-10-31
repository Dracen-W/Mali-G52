<!-- SPDX-License-Identifier: GPL-2.0-only -->
# OPEN QUESTIONS

按路线图第 1 节守则 7：不确定就停，记录在这里，向人类提问，不猜。
每条都标了卡在哪个里程碑，以及「已有答案」的条目。

## 1. Linux 源码的固定 tag / commit SHA —— 已解决：改为「内容钉死」

你的说明（直接拿源码、429 之后部分手动复制）解释了为什么这批文件与
v6.1–v6.18/master 的 sha256 都不一致：它们不是某个 tag 的快照。

- 处理方式：`third_party/PINNED.md` 改为 **content pin**——每个文件记 sha256，
  代码按「路径:函数 + 该路径的哈希」引用。文件在我们树里，哈希固定，引用不会漂，
  满足守则 1 的「不能凭记忆」；只是不声称上游身份。
- 影响：M3 的寄存器表生成和 M4 的页表摘录**不再被卡**，可以开工。
- 如果你以后想要 tag 版本，我再按指定 tag 从 git.kernel.org 重抓覆盖并更新哈希。

## 2. 中断触发标志：以 DTB 为准（电平低）—— 已解决

`tests/data/mt6768.dtb` 的 `/mali@13040000` 原文：

```
interrupts = <0x00 0x112 0x08 0x00 0x113 0x08 0x00 0x114 0x08 0x00 0x115 0x08 0x00 0x116 0x08>;
interrupt-names = "GPU", "MMU", "JOB", "EVENT", "PWR";
```

按 `include/dt-bindings/interrupt-controller/irq.h`，`IRQ_TYPE_LEVEL_HIGH = 4`，
`IRQ_TYPE_LEVEL_LOW = 8`。也就是说**这台机器的 DT 声明的是电平低**，而路线图第 2 节
写的是「GIC SPI，电平高」，两者矛盾。

- 你的决定（2026-10-25）：**以 DTB 为准**，即 `0x8` = `IRQ_TYPE_LEVEL_LOW`。
- 代码：`parseInterrupts` 仍原样记录 `type/number/flags`；新增
  `NativeMaliBoard::checkTriggers()`，要求五个源都是 SPI（type 0、编号 ≥ 32）且
  flags 为电平低，否则拒绝——把线接成边沿或反相会丢中断，甚至让线一直保持触发。
- 测试：`tests/test_board.cpp` 除断言真机 DTB 的 0x8 外，还验证把任一源改成高电平、
  边沿、PPI 或 SPI 范围外都会被拒。
- 路线图第 2 节那句「电平高」按笔误处理，不再作为依据。

## 3. 路线图的小修正：gpufreq 的 8 个 clock 不都在 topckgen —— 已查清，无需回答

DTB 里 `clock-names` 的后四项 `mtcmos_mfg_async/mfg/core0/core1` 指向
**scpsys**（phandle 0x27，`/scpsys@10001000`），不是 topckgen：

```
clocks = <0x14 0x54  0x14 0x26  0x14 0x06  0x60 0x00
          0x27 0x09  0x27 0x04  0x27 0x07  0x27 0x08>;
clock-names = "clk_mux", "clk_main_parent", "clk_sub_parent", "subsys_mfg_cg",
              "mtcmos_mfg_async", "mtcmos_mfg", "mtcmos_mfg_core0", "mtcmos_mfg_core1";
```

`0x14` = `/topckgen@10000000`（`#clock-cells = <1>`）；`0x60` =
`/mfg_cfg@13000000`（`mediatek,mfgcfg`，`#clock-cells = <1>`，即 `subsys_mfg_cg`）；
`0x27` = `/scpsys@10001000`（`#clock-cells = <1>`）。board 解析已按「名称 → phandle+id」
记录，不假设 provider。M2 会用到。

## 4. 厂商 Android 内核源码放哪里、拿哪些 —— 已落实

清单在 `docs/MTK_SOURCES.md`；源码已解压到 `third_party/mtk-android/`（gitignore，
**不入库**），M2 直接引用的九个文件在 `docs/MTK_SOURCES.md` 里记了 sha256。
M2 用到的偏移、位和电压值全部来自这批文件；缺口（MT6768 自己的 gpufreq 平台文件、
mfgsys 封装）和替代来源也写在同一份文档里。

## 5. 自制 XNU 的六个能力 —— 卡 M1 硬件探测、M7 胶水（按你的要求先标 TODO）

你说了「对标 NativeAdreno 的形式和接口，不行就标 TODO」，所以这些我按 TODO 写：
拿到 DT 属性的 API、物理 MMIO 映射、GIC/`IOInterruptEventSource` 是否可用、
缓存维护原语（clean/invalidate to PoC）、微秒定时器、IOKit 匹配与启动。
在答案到来前，`NativeMaliService` 只会是骨架，`NativeMaliPlatform` 的 IO 注入接口
先按函数指针定义（host 可用假实现），不会写任何真实 MMIO。

M5 之后补充：缓存维护已经抽象成 `NativeMaliCacheMaintainer`（`clean` /
`invalidate` / `clean+invalidate`）和 `NativeMaliMemorySource`（分配、span 表），
host 测试用假实现覆盖策略与所有权；缺的仍然只是这两个接口背后的 XNU 调用
（`IOBufferMemoryDescriptor` 的物理 span、`dc cvac`/`dc ivac` 一类的原语）。

M7 之后补充：用户态的线 ABI（`NativeMaliABI.h`）与只靠一条记录就能做的检查
（`NativeMaliAbiValidate`）已经写完并在 host 上测过，所以 XNU 能力清单里真正还
缺的只有三样：DT 属性读取、物理 MMIO 映射、GIC/`IOInterruptEventSource`，
外加 BO 背后那两条（`IOBufferMemoryDescriptor` 与缓存维护原语）。`NativeMaliService`
与 `Info.plist` 仍然要等这三样。`NativeMaliUserClient` 的纯状态机也不需要 XNU，已经写完并
在 host 上测过：`NativeMaliBoTable` 管 BO，`NativeMaliUserClient` 管 Query/
GetDeviceInfo、Submit（chain 覆盖检查、flush id、slot 选择）、fence 归属与依赖
等待、DONE/ERR/timeout 的 reap 和 fault/reset 计数、close/releaseAll 的回滚。
复位路径的软件部分也在 host 上定稿了：`NativeMaliJobSlot::quiesce()`
（每个 present 槽 `JS_COMMAND_NEXT = NOP` + `JS_COMMAND = SOFT_STOP`，再按
10 µs / 10 ms 轮询 `JOB_INT_JS_STATE`；超时只计数、不阻止复位，
`panfrost_job.c:697-709`）、`resetInterrupts()`（一次
`JOB_INT_CLEAR = ALL_JS_INT_MASK`）、`abandonOutstanding()`（纯状态丢弃）
和客户端对「被 reset 销毁的在飞 fence」返回 `IOError`。编排和 AS 所有权现在在
纯的 `NativeMaliService` 里：`openClient()` 取号、建表、`enable()`，
`closeClient()` 反序拆，`reset()` 走 `quiesce → softReset → powerOn →
resetAllocation → resetInterrupts → abandonOutstanding → noteReset →
reclaim+enable 每个 AS`，
失败时 `clientUsable()` 为假，而且失败后 `reset()` 本身也会拒绝（要 close/stop/
start 才能恢复）。所以纯逻辑层已经完整，缺的确实只有胶水层需要的那三样 XNU 能力。
复位入口也已经接上：用户态线 ABI 的 `NativeMaliMethodReset`（方法 11，无记录）
由 `NativeMaliUserClient::dispatch()` 转给 service 在 `openClient()` 时注入的
`NativeMaliResetTarget`，最终就是 `NativeMaliService::reset()`；胶水不需要额外的
sysctl/boot-arg 触发器。设备级的并发语义（任何客户端都能复位，其它客户端的在飞
fence 变成 `IOError`）已写进 `NativeMaliABI.h`，但仍未在真机上验证。

复位失败后的关闭路径**已经设计完**。原来卡住的原因是：`softReset()` /
`powerOn()` 失败后客户端仍注册，而 `releaseAll()` 要先 flush 它自己 BO 的
unmap，flush 在 `gpuReadable()` 失败时返回那个错误，于是客户端永远关不掉。
现在 `closeClient()` 先读一次 `gpuReadable()`：块不可读时走无寄存器拆除
（`NativeMaliUserClient::abandon()` → `NativeMaliBoTable::reset()`，跳过
flush 和 `disable()`），再照常 `release()` 号、`vm.reset()`、注销客户端。依据
是 `panfrost_mmu_release_ctx()` 的 `pm_runtime_active()` 守卫
（`panfrost_mmu.c:731-732`）：设备不在 runtime-active 时它也不
`panfrost_mmu_disable()`，只清分配位并释放页表。块不可读意味着域、时钟源或
时钟门至少有一个不在，GPU 不可能还在走那张表，所以直接还页是安全的。
`softReset()` 失败但 `gpuReadable()` 仍为真的情况仍走正常路径（disable +
flush），和源里「设备还 active 就照常 disable」一致；如果那个 disable 也失败，
客户端保持注册、可重试，不假装成功。

## 6. Mesa genxml v7 —— 路径已查清，尚未入库

之前给的链接（`cgit.freedesktop.org` + `src/panfrost/lib/genxml/`）在现在的 Mesa
里已经不对。Mesa `main` 的实际位置：

- `src/panfrost/genxml/v7.xml`（同目录还有 `common.xml`、`v4..v14.xml`、
  `gen_macros.h`、`gen_pack.py`、`cs_builder.h`）
- `src/panfrost/lib/pan_jc.h`：job chain，内含 `mali_job_header_packed` 和
  `write_value_index`，正是 M6 要的最小 job chain
- `src/gallium/drivers/panfrost/pan_job.*`：Gallium 提交路径

没有它，M6 只能写「提交槽位寄存器序列」（来自 `panfrost_job.c`），job chain 的
描述符布局保持 TODO，不凭记忆填。需要你点头后我把这几个文件按同样方式收进
`third_party/mesa-panfrost/` 并记 sha256。

## 7. 调试条件 —— 影响 M2/M3 敢不敢试写

已知（`docs/DEVICE.md`）：设备已 root（Magisk），装有 liveboot 模块，可以刷 boot；
因此「刷坏了再刷回去」这条路是通的。仍未知：串口或 pstore 日志能不能拿到；
有没有 9008/EDL 兜底。这决定 M2 第一次写电源寄存器时我的回滚策略。

## 11. 设备实测数据 —— 已收下

`other reference/grab_out/`（mali dmesg）与 `other reference/others/`（`/proc/iomem` 等）
已经给出：GPU 是 product `0x7002`、revision `r1p0`；Mali 窗口 `0x13040000+0x4000`；
`pwrap` 在 `0x1000d000`；设备树里没有 GPU OPP。详见 `docs/DEVICE.md`，我还在等的只有
`GPU_MMU_FEATURES`（要上机读）和 regulator summary。

## 8. UEFI 是否已把 MFG 域和时钟打开 —— 只读探测已实现，等真机输出

`NativeMaliPower::probe()` 已能读 SPM 的 `PWR_STATUS`/`PWR_STATUS_2ND`、INFRA
bus-protect、topckgen mux、mfgcfg 门控和两路 PMIC，且不配置任何寄存器。SPM 基址
`0x10006000` 已由 DT `scpsys` 的 reg 列表确认。现在缺的是在设备上跑一次：
`NativeMaliService` 和 `Info.plist` 要等 XNU 侧能力（第 5 条）确认后才写。

在拿到这组只读值并人工确认前，不打开 `NATIVE_MALI_M2_WRITES`，也不读
`0x13040000`（未上电读 MMIO 可能总线挂死，风险登记第 1 条）。

## 12. M2 上机确认清单（等 XNU 能启动）

1. 只读跑 `probe()`，记录：`spmStatus`、`spmStatus2`、`infraProtectEnable`、
   `infraProtectStatus1`、`topMfgClockConfig`、`topClockUpdate`、
   `mfgClockGateStatus`、`vgpu`/`vsram` 的原始字、电压和使能。
2. 对照预期：MFG 相关状态位（bit 11..14）是 0 还是 1（决定走路线图分支 A/B）；
   `CLK_CFG_1` 的 mfg_sel 与 gate 位；`BG3D` 状态；两路电压是否在
   500000–1293750 µV 内。
3. **人工确认**后才用 `NATIVE_MALI_M2_WRITES=1` 构建，先只调 `setFixedVoltage()`，
   再 `selectMfgClockSource()`，再 `powerOnDomains()`，最后 `enableMfgClockGate()`；
   每一步之间读一次 `probe()` 的值并记录。
4. 域上电后第一次读 `0x13040000` 走 `NativeMaliGpuCore::probe()`；它自己会先问
   `NativeMaliPower::gpuReadable()`，读不到就不碰 GPU。`GPU_ID` 必须等于
   `0x70021000`，同时把 L2/CORE/TILER/MEM/MMU/THREAD 特性字、AS/JS present、
   shader/tiler/L2/stack 位图和 `ias`/`oas` 记进 `docs/DEVICE.md`。
5. **人工确认**第 4 步的 `GPU_ID` 和特性字后，才用 `NATIVE_MALI_M3_WRITES=1`
   构建并调用 `softReset()`；它仍会先过 `gpuReadable()`。若返回
   `kIOReturnTimeout`，说明软/硬复位都没有在预算内完成，此时中断掩码保持 0，
   不要继续上电序列，把 `GPU_INT_RAWSTAT` 的原始值和日志贴回来。
6. 任一步失败：代码会回滚并把错误原样返回；不要重试、不要吞错，把日志贴回来。

## 9. 最终目标 —— 已回答

要显示 + 计算，并带上图形加速。M8（GOP 帧缓冲冒烟）和 M9（Mesa panfrost / PanVK 评估）
保留在计划里，但 M9 仍然只是评估，不作为内核里程碑的前置条件。

## 10. 电压 —— 已由 DTB 确认，无需回答

`buck_vgpu` 与 `ldo_vsram_gpu` 的 `regulator-min/max-microvolt` 都是
500000 / 1293750 µV（`0x7a120` / `0x13bdb6`），与路线图的 500000–1293750 一致。
M2 第一版固定一个保守值并硬限范围，禁止自动调压。

## 13. Mali 与 CPU 是否缓存一致 —— 已由 pinned 源与 DTB 回答：不一致

- `panfrost_device.c:269`：`pfdev->coherent = device_get_dma_attr(dev) ==
  DEV_DMA_COHERENT`，即只有设备的 DMA 属性声明一致时才设置。
- `tests/data/mt6768.dtb` 的根节点与 `/mali@13040000` 都没有 `dma-coherent`
  属性（`dtc -I dtb -O dts` 全文出现 0 次），所以 `pfdev->coherent` 为假，
  和 Linux 在这台机器上的行为一致。
- 结论（2026-10-27）：按**非一致**处理。GPU 读之前 clean，GPU 写之后
  invalidate；页表本身也要在 walker 读之前 clean（`io-pgtable-arm.c` 的
  `__arm_lpae_sync_pte()` 调用 `dma_sync_single_for_device()`，走的是同一个
  第 5 条缺口）。`NativeMaliCache.h` 把这两条路径统一成一个注入接口：
  `NativeMaliVm::prepare()` 和 `NativeMaliMemory` 都接同一个 maintainer，
  非一致时自动 clean 表页、按调用方的 `cleanForDevice()`/`invalidateForCpu()`
  维护数据页。真正的缓存原语仍是第 5 条里的未知项；在它落地之前不假设任何
  一致性，也不在 host 测试之外伪造成功。

## 14. `JOB_INT_MASK = 0` 时 `JOB_INT_RAWSTAT` 是否照样记账 —— 等真机第一条 job

M6 的完成判定选了轮询（`docs/TODO.md`）：`waitForCompletion()` 读
`JOB_INT_RAWSTAT`，`JOB_INT_MASK` 保持复位值、驱动不认领中断。推理链是
`panfrost_regs.h:233-239` 的四件套命名——`RAWSTAT`/`CLEAR`/`MASK`/`STAT`——
按 ARM Mali 的惯例，`STAT` 是 `RAWSTAT & MASK`，`MASK` 只控制中断线，事件本身
照样锁存。pinned 树里**没有一行**能证明这一点：Linux 一直在
`panfrost_jm_enable_interrupts()`（`panfrost_job.c:441-444`）里把 `MASK` 设成
`ALL_JS_INT_MASK`，从不在 `MASK = 0` 下读 `RAWSTAT`。

- 影响面：若 `MASK = 0` 时 `RAWSTAT` 不更新，`waitForCompletion()` 会对每条
  job 超时。行为仍然是失败关闭（超时不写寄存器、任务保持 outstanding、可以
  重试），但第一次真机会看起来像挂住。
- 上机确认步骤（在 M2/M3 的只读/复位确认之后，第一次 WRITE_VALUE）：
  1. 读并记录 `JOB_INT_MASK` 的复位值（预期 0；`softReset()` 不写它）。
  2. 提交之前读 `JOB_INT_RAWSTAT` 记为 A，提交之后、等待之前再读一次记为 B。
  3. 若等待超时，但 WRITE_VALUE 的目标内存确实变了（说明 job 跑了），把 A、B
     和 `JOB_INT_STAT`（`0x100c`）一起贴回来。
- 若是“不记账”：不能简单地把 `MASK` 打开，那会让一条没有处理器的电平低中断线
  一直成立。正确顺序是先做 `NativeMaliIrq`（`JOB` 线，DTB 的 `0x8` = 电平低）
  再解掩，或者确认 `MASK = 0` 时事件进的是别的寄存器。这里的决定要等实测。
- 解码部分已经写好（`NativeMaliIrq`，见 `docs/TODO.md` M3 条目），所以无论答案
  是哪一种，处理器要读的字和要清的位都不用再设计；缺的只是 XNU 侧的中断源和
  这个问题的实测答案。

## 15. merlinx 的 ADT 窗口够不到 Mali —— 先用板级常量，等 `ioreg` 验证

`UniXNU-merlinx` 在 merlinx 上用的不是 MTK 的 DTB，而是
`scripts/make-merlinx-adt.py` 生成的 Apple 格式树。它的 `/arm-io`
`ranges` 只有一段 `(0, 0x0C000000, 0x400000)`，即
`0x0C000000–0x0FFFFFFF`；Mali 在 `0x13040000`，**在窗口外**，所以
「按 DT 的 `reg` 拿地址」这条路现在走不通。

- 现在的决定（写在 `docs/INTEGRATION.md` 2.2 节）：ADT 里只加 `name` 和
  `compatible`（`mediatek,mali` / `arm,mali-valhall`，顺序照抄
  `tests/data/mt6768.dtb` 的 `/mali@13040000`），地址由 M7 的 service 用
  **板级常量**映射：`0x13040000` GPU、`0x10006000` SPM、`0x10001000`
  INFRACFG_AO、`0x10000000` TOPCKGEN、`0x13000000` MFG_CFG、`0x1000D000`
  PWRAP。每个都有哈希可查（`tests/data/SHA256SUMS`、`docs/DEVICE.md`）。
- 这不是「DTB 输给文档」，而是**这块板子的数据只在一处**：ADT 是移植层生成的，
  不是硬件描述；MTK DTB 才是。两者冲突时仍按规则让 DTB 赢，所以这里根本不
  引入第二个地址来源。
- 待回答：ADT 的 `reg` 编码是 Apple 自己的，不是标准 FDT 的
  `#address-cells × 32 位`。`intc` 那行 `struct.pack('<4Q', ...)` 是唯一被
  证明过的写法，但只证明 GIC 那一段。等 XNU 侧方便时，加第二段 `ranges` +
  `reg`，上机用 `ioreg -l -p IODeviceTree | grep -i mali` 确认驱动读到的值，
  再决定要不要把板级常量换掉。在那之前不靠猜编码。

## 16. 与 UniXNU 的对接只写文档，不动那个仓库 —— 等 XNU 侧准备好

按你的要求，这一轮**没有修改** `~/Desktop/merlinx-work/UniXNU-merlinx`。
需要的三处改动（ADT 节点、`build-arm-driver.py` 的三处映射、`board.toml` 一行）
加上子模块和 `Info.plist`，全部写进 `docs/INTEGRATION.md`，可以照抄。

前置条件在本仓库：`xnu/` 里的胶水已经存在，但从未按真正的内核 ABI 编译过
（M7，卡在第 5 条的 XNU 能力和内核构建树上）；`Info.plist` 是纸面文件，
只有 kext 路线才用它。在那之前加 `build-arm-driver.py NativeMali` 会让
`-Werror` 构建直接失败，所以文档里把顺序写成「先本仓库 M7，再 UniXNU」。
具体要准备什么写进了 `docs/KEXT_BUILD.md`。

