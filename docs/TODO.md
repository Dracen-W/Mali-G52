<!-- SPDX-License-Identifier: GPL-2.0-only -->
# TODO（有意未实现，不是遗漏）

每一条都写了「为什么现在不写」。路线图守则 1（来源可追溯）和守则 7（不确定就停）
是主要原因；另一部分是硬件还没到。

## 剩余缺口一览（索引）

这张表只做索引：每条的理由、来源和验证方法都写在下面的里程碑小节里。表里
「已接上」表示代码路径存在并有 host 测试，**不**表示已在手机上验证。

| 缺口 | 现在到哪一步 | 关闭条件 |
| --- | --- | --- |
| 复位触发 | **已接上**：ABI 方法 11 `NativeMaliMethodReset`（无记录）→ `NativeMaliUserClient::dispatch()` → service 注入的 `NativeMaliResetTarget` → `NativeMaliService::reset()`；任何已连接的客户端都能调用，其它客户端的在飞 fence 变成 `IOError` | 在真机上复位一次，确认 `GetStatus::resets` 前进、设备继续可用，再删掉 `docs/UNVERIFIED.md` 的对应条目 |
| 真机只读 probe 验收 | M1–M3 代码齐，`NATIVE_MALI_M2_WRITES`/`M3_WRITES`/`M4_WRITES`/`M6_WRITES` 默认 0；从未在手机上跑过 | 串口/日志里人工确认 probe 读数，记录 `buck_vgpu` 初值、`GPU_INT_RAWSTAT` 复位值、特性字和 present 位图，再逐个打开写开关 |
| XNU 能力 | DT 属性读取、物理 MMIO 映射、`IOInterruptEventSource`、缓存维护原语、kext 编译/打包；`xnu/` 只有纸面实现和本地桩语法检查 | 在 UniXNU-merlinx 树里编译成 kext、`ioreg` 看到实例、`start()` 日志与 probe 打印出来（`docs/OPEN_QUESTIONS.md` 第 5 条） |
| 中断驱动的完成通知 | `NativeMaliIrq` 只解码；完成仍是 `waitForCompletion()` 轮询，`JOB_INT_MASK` 从不解掩 | 有 `IOInterruptEventSource`、确认 `JOB_INT_MASK` 为 0 时 `RAWSTAT` 仍记账（`OPEN_QUESTIONS` 第 14 条），再把 fence 变成唤醒而非阻塞 |
| 故障日志与 mask | `NativeMaliFault`/`NativeMaliIrq` 的解码与只读故障寄存器齐；可读日志、每次 fault 后的 `GPU_INT_MASK = 0`/`MMU_INT_MASK` 处理没有挂载点 | 同上，接上中断处理器；page fault 的按需映射等有真机 job 后再做 |
| AS 满载的 LRU 回收 | `NativeMaliMmu::allocate()` 满载直接 `NoResources`（`panfrost_mmu_as_get()` 会复用最久未用） | 有多客户端同时提交的需求且能证明复用安全；现在只有 3 个 AS，优先保证不互相偷表 |
| 2 MiB / 1 GiB 块映射与空表回收 | 只有 4 KiB 叶；unmap 后不回收空表，所以大 buffer 的 PTE 比 panfrost 多 | 性能阶段，先有真机 benchmark 和页表预算压力 |
| 用户态客户端 / Mesa | 线 ABI 与纯状态机齐；没有用户态库、没有 Mesa 后端（M9） | 内核路径在真机上闭环后评估工作量 |
| 挂起/恢复、空闲关核、CI、逐文件许可证审计 | 未做（M10） | 真机稳定后 |

## M1–M4 编排（`NativeMaliBringup`）

- [x] **一个调用把 M1 到 M4 按源顺序串起来**：`start()` 先跑 M1 的只读
      `probe()`（读不到就一个配置寄存器都不写），然后 M2（电压 → 时钟源 → 四个域
      → 时钟门），再 M3（`probe()` → `softReset()` → `powerOn()`），最后 M4
      （`setPresent()` → `resetAllocation()`）。顺序的依据是
      `panfrost_device_init()`（`:276-320`，gpu_init 在 mmu_init 之前）和
      `panfrost_device_reset()`（`:510-524`，soft_reset → power_on → mmu_reset），
      写在 `NativeMaliBringup.h` 的注释里。`stop()` 是
      `panfrost_device_fini()`（`:388-408`）的反序：GPU 下电 → M2 `tearDown()`；
      有 job 在飞时按 `panfrost_device_runtime_suspend()`（`:553-554`）返回
      `Busy` 且零写。
      失败一律逆序回滚，返回**原始错误**，回滚错误走 `teardownError`。
      `tests/test_bringup.cpp` 用共享序号计数器证明**跨块**顺序（M2 的门先于第一个
      GPU 写、复位先于上电、客户端的 AS disable 早于 GPU 下电）。
- [x] **AS 不归这一层**：`start()` 只记 `GPU_AS_PRESENT` 并清掉上一次启动可能
      留下的分配位（`panfrost_mmu_reset()`，`panfrost_mmu.c:345-365`），不分配、
      不编程。地址空间属于页表的主人：源里 `panfrost_job_hw_submit()` 在提交时取
      （`panfrost_job.c:222`）、DONE/ERR 时还（`:494`、`:515`），客户端关闭时
      `panfrost_mmu_release_ctx()` disable 并清位（`panfrost_mmu.c:722-743`）。
      本驱动把「每次 job 取还」简化成「客户端打开到关闭」，一次编程而不是两次，
      所以谁打开客户端谁就 `allocate()`+`enable()`，关闭时 `disable()`+`release()`。
      只要分配位非零，`stop()` 就返回 `Busy` 且零写——不能在 GPU 还可能走那张表
      的时候把电断掉。`tests/test_bringup.cpp` 锁住这一点。
- [ ] **job slot 与 VM 不在这层**：槽位属于提交的客户端，页表来自调用方的
      `NativeMaliPageSource`，所以 `start()` 既不碰槽位也不建表，`stop()` 的
      `Jobs` 参数仍是调用方声明（现在那个调用方是 service，它自己知道槽位）。
- [x] **service 的纯逻辑部分（`NativeMaliService`）**：把 bringup、共享的
      `NativeMaliJobSlot` 和「一个客户端一个 AS」放进一个对象；不碰新寄存器，
      只排顺序和所有权。
      - `prepare()` 把块交给 `NativeMaliBringup`，再把 job slot prepare 在同一份
        `power`/`mmu`/GPU 窗口上（不是副本），因为它们本来就共享
        `0x13040000`。
      - `start()` = bringup 的 M1→M4，然后 `setPresent(GPU_JS_PRESENT,
        GPU_SHADER_PRESENT)`（源里 `panfrost_jm_init()` 的读取，
        `panfrost_device.c:320`）；后者失败就逆序把块关掉。还有客户端没关时
        拒绝，避免在旧 AS 上再起一次。
      - `openClient()`：`allocate()` 取最低空位 → `vm.prepare()`（几何用 M3 probe
        的 `ias`/`oas`，和 `panfrost_mmu_ctx_create()` 一样）→ `mmu.enable()`
        → `client.open()`。前两步失败就把号还回去；`enable()` 之后的失败按
        `panfrost_mmu_release_ctx()`（`panfrost_mmu.c:722-743`）反序
        disable → release → `vm.reset()`，**原错误**返回，回滚错误走
        `teardownError`。disable 失败时刻意不还号（GPU 可能还在走那张表）。
      - `closeClient()`：块可读时先 `releaseAll()`（它会等本客户端在飞的
        job），再 disable → release → `vm.reset()`；任一步失败就保持注册、
        保持 AS，可以重试，`stop()` 期间一直 `Busy`。块不可读
        （`gpuReadable()` 失败，复位后电源没回来就是这种情况）时走无寄存器
        拆除：`abandon()` 直接把 BO 还回 source，跳过 flush 和 disable，再
        release 号、`vm.reset()`、注销。依据是 `panfrost_mmu_release_ctx()`
        的 `pm_runtime_active()` 守卫（`panfrost_mmu.c:731-732`），见
        `docs/OPEN_QUESTIONS.md`。
      - `stop()`：还有客户端（也就是还有分配的 AS）或任何槽还 outstanding 时
        `Busy` 零写；否则 `NativeMaliBringup::stop()` 的逆序下电。正常路径里
        job 不会比客户端活得久（槽位拒绝未分配 AS 的提交，`closeClient()` 先等
        本客户端的 job），job 检查是给「AS 被提前还回去」这类 owner bug 兜底的。
      - `reset()`：`quiesce()`（每个 present 槽 `JS_COMMAND_NEXT = NOP` +
        `JS_COMMAND = SOFT_STOP`，再每 10 us 轮询 `JOB_INT_JS_STATE`，最多
        10 ms，`panfrost_job.c:697-709`）→ `softReset()` → `powerOn()` →
        `resetAllocation()` → `resetInterrupts()` → `abandonOutstanding()` →
        每个客户端 `noteReset()` → 每个 AS `reclaim()` + `enable()`。quiesce
        之后和 `panfrost_device_reset()`（`panfrost_device.c:510-524`）同序。
        soft-stop 超时不是失败：源里只打 "Soft-stop failed" 然后照样 reset
        （`panfrost_job.c:708-709`），这里计数到 `softStopTimeouts()` 并继续；
        寄存器错误则原样返回、在碰复位线之前中止。任何其他失败都让
        `clientUsable()` 为假，只有 close/stop/start 能恢复。失败路径也先把
        每个 AS 号 `reclaim()` 回来：号还是那个客户端的，而且失败的 reset 之后
        `closeClient()` 还要靠它 unmap 和拆表。
      - 不做的三件事，各有理由：`drm_sched_stop()` 和给在飞 fence 打错误由
        `abandonOutstanding()`/`noteReset()` 纯状态替代（没有 scheduler/job
        对象）；`JOB_INT_MASK = 0` 不需要（本驱动从不解掩）；
        `drm_sched_resubmit_jobs()` 没做（重发会让已经交出去的 fence 变成谎言，
        调用方拿到错误 fence 自己决定）。`panfrost_jm_enable_interrupts()`
        同样不需要，见 M6 的轮询决定。
      `tests/test_service.cpp` 用真实 bringup、真实页表、真实 MMU 编程、真实
      client submit 锁住以上全部；24 个 mutation 各自打在不同断言上，quiesce/
      soft-stop 这一轮又加 15 个（9 个落在 `test_job_slot`，6 个落在
      `test_service`），全部被抓住。
- [ ] **IOKit 胶水（`NativeMaliGpuService`、`NativeMaliGpuClient`、
      `Info.plist`）**：纸面版本已写在 `xnu/`，能在本地桩头文件下通过语法检查，
      但**从未按真正的内核 ABI 编译**。构建需要什么见 `docs/KEXT_BUILD.md`；
      XNU 侧能力仍缺第 5 条那几项。

## M1 平台侦察

- [x] **SPM 只读探测**：`NativeMaliPower::probe()` 读 `PWR_STATUS`/`PWR_STATUS_2ND`、
      INFRA protect、topckgen `CLK_CFG_1`、mfgcfg 门状态和两路 PMIC 电压/使能，
      不改任何配置寄存器。偏移取自 `clk-mt6768-pg.c`，SPM 基址 `0x10006000`
      由 DT `scpsys` 的 reg 列表确认（`docs/DEVICE.md` 第 3 节）。
- [ ] **在真机上跑一次 probe**：需要驱动先进内核（内置驱动或 kext，见
      `docs/KEXT_BUILD.md`），而胶水依赖 XNU 侧能力（下一条）。在拿到串口输出并人工确认前，不打开
      `NATIVE_MALI_M2_WRITES`，也不读 `0x13040000`。
- [ ] **XNU 侧取 DT 属性的 API**：`NativeMaliService` 里会像 NativeAdreno 那样
      `getProperty("reg")`，但自制 XNU 是否已把 DT 属性挂到 `IOPlatformDevice` 上，
      待确认（`docs/OPEN_QUESTIONS.md` 第 5 条）。
- [ ] **物理 MMIO 映射与 GIC 注册**：同上，按 TODO 标记，接口先按 NativeAdreno 的形状
      预留（`IODeviceMemory::map` + `IOInterruptEventSource`）。
- [ ] **`Info.plist`**：`xnu/Info.plist` 已有一份纸面文件，`IOClass` 是代码里
      确实存在的 `NativeMaliGpuService`，但 `IOProviderClass`、匹配规则和 bundle id
      都未验证，也从未安装。内置驱动不使用它；kext 路线才需要它，见
      `docs/KEXT_BUILD.md` 第二节。

## M2 电源、时钟、供电

- [x] 电压范围已由 DTB 确认：500000–1293750 µV，第一版固定保守值、硬限范围。
- [x] **电压**：`NativeMaliPwrap`（WACS2）+ `NativeMaliMt6358`（VGPU/VSRAM）。
      固定 `VGPU 612500 µV`（selector 18）、`VSRAM_GPU 850000 µV`（selector 56），
      即厂商 OPP 表最低档；VSRAM 先于 VGPU；每次写后回读，不一致就恢复旧值并报错。
- [x] **时钟源**：topckgen `CLK_CFG_1` 的 mfg mux 切到 clk26m（26 MHz），释放 mux
      gate，脉冲 `CLK_CFG_UPDATE` 的 mfg 位；不碰 MFGPLL/APMIXED。
- [x] **时钟门**：`mfg_cfg` 的 `BG3D`（偏移 `0x8` 写 1），写后回读状态位。
- [x] **四个域**：`async → mfg → core0 → core1`，每个域按厂商 `PWR_CON` 步骤；
      MFG 的 bus-protect 在域上电之后释放，关电时先 assert 再断电
      （`clk-mt6768-pg.c:3242/3269` 与 `:3295/3319`）。
- [x] 失败即回滚：域按反序关闭；电压回读失败恢复原字；门控没打开则把已开的域
      全部关掉，不报告 ready。写入默认编译关闭（`NATIVE_MALI_M2_WRITES=0`）。
- [x] **下电顺序**：`powerDownDomains()` 按厂商 `mt_gpufreq_disable_MTCMOS()`
      （`mtk_gpufreq_core.c:466-496`，四个调用在 `:478-481`）的顺序
      `core1 → core0 → mfg → async`，正好是上电的反序（也是
      `clk-mt6768-pg.c:3477-3485` 的父时钟层级）。`disableMfgClockGate()` 是
      厂商 `clk_disable_unprepare(subsys_mfg_cg)`（`mtk_gpufreq_core.c:415-432`）
      的等价：往 `MFG_CFG` 的 `SET` 写 1 再回读状态位（`clk-gate.c:167-175`
      定义 set/clr 门的状态位为 1 即已关）。`tearDown()` 先把门关掉再关四个域，
      与厂商 `disable_CG` 在 `disable_MTCMOS` 之前一致。
      每一步都尝试，返回**第一个**错误（形状同 `panfrost_gpu_power_off()`，
      `panfrost_gpu.c:486-504`）；失败的步骤保持“还在”，所以可以重试，
      `started()` 在此期间不会说谎。
- [ ] **刻意不镜像的两步**：厂商 `__mt_gpufreq_bucks_disable()`
      （`mtk_gpufreq_core.c:2036-2068`）会关 VGPU 再关 VSRAM_GPU，但本驱动从没
      打开过这两路（`setFixed()` 只写 VOSEL，两路是 bootloader 开的），关掉就
      没有一条有来源的路径能再打开；topckgen 的 mfg mux 门厂商只在换父时钟前后
      短暂打开（`:1871-1900`），而 `gpuReadable()` 把“门开着”当作“0x13040000
      可读”，在这里关掉会改变 M1 探测的含义。两条理由写在
      `NativeMaliPower.h` 的 `tearDown()` 注释里，等 XNU 侧确认电源域所有权
      之后再决定是否补。
- [x] 不需要 DVFS：设备实测显示 DDK 在设备树里找不到 GPU OPP（`docs/DEVICE.md` 第 4 节），
      固定电压 + 固定时钟就是这台机器的现状。
- [ ] **MFGPLL 与更高频率**：当前只选 26 MHz 源，不编 PLL。等 M3 能读 GPU 状态、
      且需要真实性能时再按 `clk-mt6768.c` 的 PLL 表补；在那之前高频率不是需求。
- [ ] **`buck_vgpu` 的实际初始电压**：`regulator_summary` 还没拿到；probe 会读回
      实际值，正好可以在第一次上机时补上这条事实。

## M3 GPU 核心

- [x] **`NativeMaliRegs.h` 生成脚本**：`tools/generate-regs.sh` 从 `panfrost_regs.h`
      逐行复制，源文件哈希与 `third_party/PINNED.md` 不符就拒绝生成；`tests/run.sh`
      每次重新生成并比对，`tests/test_regs.cpp` 锁住关键偏移。
- [x] **GPU_ID 解码**：`NativeMaliGpu::identify()`，产品号/修订号拆分、T60x 修正、
      产品号掩码、Bifrost 型号表与修订条目，未知识别码拒绝而不是猜。
      真机值已知：`0x70021000` → g52/r1p0（`docs/DEVICE.md` 第 2 节），
      `tests/test_gpu.cpp` 已把它作为黄金用例。
- [x] **PRESENT/FEATURES 读取**：`NativeMaliGpuCore::probe()` 读 `GPU_ID`、L2/CORE/
      TILER/MEM/MMU/THREAD 特性字、thread 上限、AS/JS present、每个 present job slot
      的 `JS_FEATURES`、四组 64 位 present 位图（LO | HI << 32）和 `THREAD_TLS_ALLOC`；
      计数用 popcount，`ias`/`oas` 从 `GPU_MMU_FEATURES` 取。它先调用
      `NativeMaliPower::gpuReadable()`，域/时钟门没开时**一个 GPU 寄存器都不读**。
      `tests/test_gpu_core.cpp` 用合成特性字验证管线；真实值等上机。
- [ ] **真机特性值**：M2/M3 上机后把 `probe()` 打印的 `L2/CORE/TILER/MEM/MMU/THREAD`
      与 present 位图记进 `docs/DEVICE.md`，再决定 `ias`/`oas` 是否可以交给
      `NativeMaliVm::prepare()`。
- [x] **软复位**：`NativeMaliGpuCore::softReset()` 按 `panfrost_gpu_soft_reset()`
      （`panfrost_gpu.c:62-108`）的顺序：`INT_MASK = 0`、`INT_CLEAR =
      GPU_IRQ_RESET_COMPLETED`、`GPU_CMD = GPU_CMD_SOFT_RESET`，以 10 µs 间隔轮询
      `GPU_INT_RAWSTAT` 的 reset 位；超时就写 `GPU_CMD_HARD_RESET`，改为 100 µs
      间隔再轮询；两次都超时返回 `kIOReturnTimeout` 且不再继续。成功后
      `INT_CLEAR = GPU_IRQ_MASK_ALL`，再 `INT_MASK = ERROR | PERFCNT |
      CLEAN_CACHES`（与源一致）。写入在 `NATIVE_MALI_M3_WRITES`（默认 0）之后，
      并先过 `gpuReadable()`。`tests/test_gpu_core.cpp` 锁住写序、轮询间隔、
      hard 回退和失败传播。
- [x] **L2/shader/tiler 上电/下电**：`NativeMaliGpuCore::powerOn()` 按
      `panfrost_gpu_power_on()`（`panfrost_gpu.c:455-484`）：`L2_PWRON_LO`、
      `SHADER_PWRON_LO`（两者都先与 `panfrost_get_core_mask()` 取的第一组核
      `~(l2-1) & (l2-2)` 相与）、`TILER_PWRON_LO`（不做 core mask），每一步
      之后都以 10 µs 间隔轮询对应的 `*_READY_LO` 直到**等于**目标掩码；预算
      分别是 20 ms、20 ms、1 ms。`powerOff()` 对应 486-504 行：shader、tiler、L2
      依次 `*_PWROFF_LO` 并轮询 `*_PWRTRANS_LO` 归零（shader/tiler 1 µs、
      L2 不睡，预算都是 2 ms），某一步失败也继续关其余块并返回第一个错误。
      上电前要求 `probe()` 成功且 `softReset()` 成功；任一步失败就 `powerOff()`
      回滚并清掉 reset 状态；下电后同样要求重新复位。只写 LO 半，present
      位图有 bit31 以上或某类为 0 时直接 `Unsupported`，不写半个 GPU。
      `init_quirks()` 不在这条路径里：它依赖 model feature/issue 表，而那张表
      还没移植，且它配的是 job/cache 而不是电源握手。`tests/test_gpu_core.cpp`
      锁住写序、第一组核、轮询次数/间隔、拒绝和回滚。
- [x] **init_quirks**：`powerOn()` 在第一个 PWRON 之前按
      `panfrost_gpu_init_quirks()`（`panfrost_gpu.c:117-180`）写一次
      `GPU_JM_CONFIG = JM_DEFAULT_IDVS_GROUP_SIZE << JM_IDVS_GROUP_SIZE_SHIFT`
      （`0x000F0000`）。只写这一处是有依据的：G52 的 feature 表有
      `HW_FEATURE_IDVS_GROUP_SIZE`（`panfrost_features.h:73-83`），而
      `hw_issues_g52` 为空、`hw_issues_all` 只有 `HW_ISSUE_9435`
      （`panfrost_issues.h:141-142,248`），本函数检查的
      `8443/11035/10327/10797/TTRX_2968/T76X_3953/GPUCORE_1619` 都不在其中；
      model 比较分支又是 T60x..T8xx 的，G52（0x7002）不命中。所以 `SC_*` 全为 0
      （不写 `GPU_SHADER_CONFIG`），`TC_CLOCK_GATE_OVERRIDE` 也不需要——源里那句
      无条件的 `GPU_TILER_CONFIG` 回写只会写回刚读到的值，故跳过。写入仍受
      `NATIVE_MALI_M3_WRITES` 和 `gpuReadable()` 约束，host 测试锁住位置和值。
- [ ] 复位后的 `INT_RAWSTAT` 初值记录：第一次上机把 `GPU_INT_RAWSTAT` 的原始值
      记进 `docs/DEVICE.md`，确认硬复位是否像 panfrost 期望的那样留下
      `RESET_COMPLETED`。
- [ ] `NativeMaliIrq`：三条中断的应答与解码；EVENT/PWR 保持屏蔽，只记录。
- [ ] 每核的 issue 位表（`panfrost_issues.h`）：确认 G52 的修复项后再写，现在
      `identify()` 只报告是否命中了某个修订条目。

## M4 MMU 与页表

- [x] `NativeMaliVm`：ARM_MALI_LPAE 页表构造，从 `io-pgtable-arm.c` 摘出并去掉
      Linux 依赖。几何、描述符编码、表共享、拒绝路径、表上限和随机 map/walk/unmap
      都在 `tests/test_vm.cpp` 里（`tests/run.sh --san` 也过）。
- [x] **`NativeMaliMmu` 槽位与使能/关闭**：`setPresent()` 记录 `GPU_AS_PRESENT`，
      `allocate()` 取第一个空闲且存在的位（`panfrost_mmu_as_get()` 的 ffz 语义；
      满载时拒绝，不做 LRU 回收），`release()` 归还。`enable()` 按
      `panfrost_mmu_enable()`（`panfrost_mmu.c:167-190`）：整段 `FLUSH_MEM`
      （先写 `AS_LOCKADDR`、发 `LOCK`、发 `FLUSH_MEM`、再等 `AS_STATUS` 空闲），
      然后 `AS_TRANSTAB`/`AS_MEMATTR`（值来自 `NativeMaliVm`）与
      `AS_TRANSCFG = ADRMODE_LEGACY`，最后 `UPDATE`；任何配置写失败都会尽力
      `disable()` 再返回原错误。`disable()` 对应源文件的 191-205 行。每次命令前
      按 `AS_STATUS_AS_ACTIVE` 以 10 µs 间隔轮询，预算 100 ms。写入在
      `NATIVE_MALI_M4_WRITES`（默认 0）之后，并先过 `gpuReadable()`。
      `tests/test_mmu.cpp` 锁住 `AS_LOCKADDR` 编码、写序、轮询、拒绝与回滚。
- [x] **GPU 复位后的槽位记忆**：`resetAllocation()` 清掉分配位图，对应
      `panfrost_mmu_reset()`（`panfrost_mmu.c:345-365`）里的
      `as_alloc_mask = 0` / `as_faulty_mask = 0` / 每个 `mmu->as = -1`。一次 GPU
      复位把硬件里编好的 AS 全抹掉了，所以驱动这边的“谁占了哪个槽”也不再成立。
      `present()` 不清：`GPU_AS_PRESENT` 是硬件事实，不是分配记录。源文件在这里
      还会重开 MMU 中断，本驱动的中断路径还没写（M3 的待办），所以只做记账。
      `panfrost_device_reset()` 把它放在 `panfrost_gpu_power_on()` 之后
      （`panfrost_device.c:517`），`NativeMaliBringup::start()` 照这个顺序。
      `tests/test_mmu.cpp` 锁住“清分配、留 present、复位后仍从低位分配、零寄存器访问”。
- [x] **故障解码**：`NativeMaliFault` 是纯逻辑（无 IO）：`exceptionName()` 按
      `panfrost_device.c:425-500` 的稀疏表逐码对照（0xF0 以上与空档都返回
      `"Unknown exception type"`），`isFault()` 是 `code > 0x3F`
      （`panfrost_device.h:331-335`），`accessName()` 按 `access_type_name()`
      （`panfrost_mmu.c:847-869`；G52 有 `HW_FEATURE_AARCH64_MMU`，所以 access 0
      是 ATOMIC）。`NativeMaliGpuCore::readFault()` 读 `GPU_FAULT_STATUS` 与
      `GPU_FAULT_ADDRESS_HI/LO`（`panfrost_gpu.c:32-35`），
      `NativeMaliMmu::readFault()` 读 `AS_FAULTSTATUS` 与
      `AS_FAULTADDRESS_LO/HI`（`panfrost_mmu.c:897-899`），两者都只读、都先过
      `gpuReadable()`；MMU 侧还要求该 AS 的 present 位存在。`tests/test_fault.cpp`
      锁住整张表，两个读路径分别在 `test_gpu_core.cpp` / `test_mmu.cpp` 里测。
- [x] **中断状态解码**：`NativeMaliIrq` 只做解码，不读寄存器、不认领中断、不写
      任何东西。`decodeGpu()` 是 `panfrost_gpu_irq_handler()`（`panfrost_gpu.c:24-60`）
      读的那一个 `GPU_INT_STAT` 字：`GPU_IRQ_FAULT | GPU_IRQ_MULTIPLE_FAULT` 才算
      `error()`（源里只有这一对会去读故障记录、打日志、把 `INT_MASK` 清零），
      复位完成 / 功耗变化 / 功耗全变 / perfcnt 采样 / 清缓存完成各有自己的
      accessor，`GPU_IRQ_MASK_ALL` 之外的位留在 `unknown` 里（源里照样会写回
      `GPU_INT_CLEAR`，所以不能丢）。`decodeJobs()` 把低半的字
      `JOB_INT_MASK_DONE(js) = BIT(js)` 与高半的 `BIT(js + 16)` 折成「每个 slot 一位」
      的两张表，`NUM_JOB_SLOTS`（`panfrost_job.h:48`）之外的位进 `unknown`——那是
      硬件没有的 slot，源里会照样去 `panfrost_dequeue_job()`。`decodeMmu()` 复现
      `panfrost_mmu.c:889-890` 的配对 `BIT(as)` / `BIT(as + 16)`：两半分开报（源里
      没有给高半起名，就不替它起名），并把 `GPU_AS_PRESENT` 里没有的 AS 单独放进
      `absent`，`mmuClearMask(as)` 就是那个清位字。`tests/test_irq.cpp` 锁住全部
      映射与保留位。
- [ ] **故障上报（日志与 mask）**：可读的 `"%s at 0x%016llX"` 日志与每次 fault 后的
      `GPU_INT_MASK = 0` / `MMU_INT_MASK` 处理要挂在真正的中断处理器上，那需要
      XNU 侧的中断源（第 5 条），所以留给 M7；解码已经就位。page fault 的按需映射
      （`panfrost_mmu_map_fault_addr()`）留到有真机 job 之后。
- [x] **映射更新的 FLUSH_PT**：`NativeMaliMmu::flushRange()` 按
      `panfrost_mmu_flush_range()`（`panfrost_mmu.c:389-404`）先锁住
      `[start, start+size)` 再发 `AS_COMMAND_FLUSH_PT`，然后等 `AS_STATUS` 空闲；
      这是 `NativeMaliVm` 改完页表后真正要走的路径。size 0 拒绝（源里的
      `lock_region()` 对 0 不写，等于刷一个空范围）。`AS_COMMAND_UNLOCK` 在 pinned
      源里没有任何调用者，所以不凭空实现一个单独解锁。槽位必须 present 且已
      allocate，因为使能过才谈得上刷它的表。
- [ ] **AS 满载时的 LRU 回收**：`panfrost_mmu_as_get()` 会复用最久未用的槽位，
      现在直接返回 `NoResources`；等 M6 有 job、需要多客户端并发时再补。
- [ ] **MMU 中断寄存器**：`MMU_INT_CLEAR/MASK/STAT` 故意未动。panfrost 在
      `panfrost_mmu_init()` 里全开，而这个驱动还没有中断处理路径，先保持屏蔽。
- [ ] 2 MiB / 1 GiB 块映射、取消映射后回收空表：现在只做 4 KiB 叶，够 M6 的第一次提交，
      性能阶段再补。
- [x] **页表写回**：非一致 walker 的 PTE 发布是 `io-pgtable-arm.c` 的
      `__arm_lpae_sync_pte()`/`__arm_lpae_clear_pte()` 加上表页分配时的
      `dma_map_single()`。`NativeMaliVm::prepare()` 现在可以接一个
      `NativeMaliCacheMaintainer`：给了就在 prepare()（空 PGD 先于 TRANSTAB，
      见 `io-pgtable-arm.c:1263` 的 `wmb()`）、map()、unmap() 之后把**每个**表页
      clean 一次（比源里按 PTE 批量同步更保守，clean 是幂等的）；发布失败时
      map() 撤回刚写的范围并返回原错误，unmap() 只返回错误、重试再发布一次。
      不给 maintainer 时仍由调用方负责，M4 的旧测试走这条路。真正的 XNU 缓存
      原语仍是 `docs/OPEN_QUESTIONS.md` 第 5 条。
- [ ] AArch64 4K 页表格式（`ARM_64_LPAE_S1`）：panfrost 只对设置了
      `GPU_QUIRK_FORCE_AARCH64_PGTABLE` 的板子（mt8188/mt8192/mt8370）启用，MT6768 不在其中，
      所以不实现。若将来换板再补（包括 `mair_to_memattr()`）。

## M5 内存对象与缓存维护

- [x] **CPU/GPU 一致性结论**：`panfrost_device.c:269` 只在设备 DMA 属性是
      `DEV_DMA_COHERENT` 时把 `pfdev->coherent` 置真；`tests/data/mt6768.dtb`
      的根节点和 `/mali@13040000` 都没有 `dma-coherent`（`dtc` 全文 0 次），
      所以本机是**非一致**的。策略按路线图：GPU 读之前 clean，GPU 写之后
      invalidate。结论记在 `docs/OPEN_QUESTIONS.md` 第 13 条。
- [x] **`NativeMaliMemory`**：一个 buffer object = CPU 指针 + 物理 span 表 +
      （可选）在某个 AS 里的映射。`prepare()` 从 `NativeMaliMemorySource` 拿
      内存并校验 span：4 KiB 对齐、页整数倍、从 0 连续、恰好覆盖整个 buffer、
      物理地址不回绕；任何一条不满足就原样还回去并拒绝。非一致 buffer 没有
      `NativeMaliCacheMaintainer` 时拒绝 prepare——静默跳过 clean/invalidate
      等于映射一块 CPU 与 GPU 看到的内容可能不同的内存。
      `map()` 按 `panfrost_mmu_map()`（`panfrost_mmu.c:466-493`）逐 span 映射、
      整段 flush 一次（`NativeMaliMmu::flushRange()`），默认 prot 是
      `IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE`；失败时按 `mmu_map_sg()`
      （`panfrost_mmu.c:421-464`）撤回已映射的部分并返回原错误。
      `unmap()` 清叶再 flush 一次；flush 失败时保持“已映射”，因为叶子已清、
      重试只会再 flush 一次。`cleanForDevice()`/`invalidateForCpu()` 是策略层：
      一致的 buffer 是 no-op，非一致的交给注入的 maintainer。
      `tests/test_memory.cpp` 覆盖分配/释放、span 校验、每 span 的物理地址黄金
      值、整段 flush 的 `AS_LOCKADDR` 值、重复映射、回滚、flush 失败、缓存范围
      与 no-op。
- [ ] **XNU 后端**：`NativeMaliMemorySource` 的 `IOBufferMemoryDescriptor`
      实现（分配、固定、CPU 映射、span 表）和 `NativeMaliCacheMaintainer` 的
      真实缓存原语。两者都卡在 `docs/OPEN_QUESTIONS.md` 第 5 条；接口已经定好，
      host 测试用假实现。
- [x] **页表缓存发布**：`NativeMaliVm::prepare()` 现在可以接
      `NativeMaliCacheMaintainer`，非一致 walker 的表页在 prepare()/map()/
      unmap() 之后自动 clean（细节见 M4 那条）。把同一个 maintainer 传给
      `NativeMaliMemory::map()` 用的 `NativeMaliVm`，页表与数据就共用一套
      DMA 可见性；缺的只是 maintainer 背后的 XNU 原语（第 5 条）。
- [ ] 可增长堆、tiler heap：按路线图不做；2 MiB / 1 GiB 块映射也不做，所以
      大 buffer 的页表项比 panfrost 多。

## M6 Job 与 fence

- [x] **Job 描述符布局**：Mesa 的 `v7.xml` 与 `pan_jc.h` 已按 sha256 收进
      `third_party/mesa-panfrost/`。`NativeMaliJob` 逐位打包 Job Header
      （`v7.xml:1334-1351`，字 0-7：Exception Status、First Incomplete Task、
      Fault Pointer、带 Is 64b/Type/Barrier/Invalidate Cache/Suppress Prefetch/
      Enable Texture Mapper/两个 Relax Dependency/Index 的标志字、两个
      Dependency、Next）、Job Type（`v7.xml:37-49`）和 WRITE_VALUE 的
      Type/Payload（`v7.xml:1370-1399`：Address 字 8-9、Type 字 10、立即数
      字 12-13，aggregate 要求 Payload 在偏移 32）。链的规则来自
      `pan_jc_add_job()`：索引是 `++job_index`（第一个 job 是 1），新 job 的
      GPU 地址写进前一个 job 的 Next，最后一个保持 0。`tests/test_job.cpp`
      锁住黄金字（第一个 WRITE_VALUE 的标志字 `0x00010005`）、payload 布局、
      64 字节槽对齐、链接、只有 Immediate* 带立即数、容量耗尽与 16 位 Index
      上限。
      只做纯打包：内存由调用方给（`cpu` 与 `gpu` 两个地址），不分配、不碰
      MMIO，也不需要新开关。
- [x] **JS 槽位提交**：`NativeMaliJobSlot` 按 `panfrost_job_hw_submit()`
      （`panfrost_job.c:205-282`）写 `JS_HEAD_NEXT_LO/HI` →
      `JS_AFFINITY_NEXT_LO/HI`（`features.shader_present`，「先给所有核」）→
      `JS_CONFIG_NEXT` → `JS_FLUSH_ID_NEXT` → `JS_COMMAND_NEXT = START`。
      `JS_CONFIG` 的值就是源里的 `cfg`：AS 号在低 4 位（源里唯一的描述是
      `cfg & 0xf` 的 `AS %d` 打印）、`THREAD_PRI(8)`、起止各一次
      clean+invalidate、`ENABLE_FLUSH_REDUCTION`（`hw_features_g52` 有这一项，
      所以 `JS_FLUSH_ID_NEXT` 一定要写，值来自 `GPU_LATEST_FLUSH_ID`），
      以及链标志。`START_MMU` 不设，因为它只在 `HW_ISSUE_10649` 时设，而
      `hw_issues_g52` 是 0。链标志按每个槽的 `emit_seqno` 奇偶取（源里
      `fence->seqno = ++queue[js].emit_seqno`，第一个 job 是 1），所以同一个槽
      连续两次提交的标志相反。
      失败关闭：写之前先过 `gpuReadable()`、槽必须在本驱动用的 3 个槽
      （`NUM_JOB_SLOTS`）和 `GPU_JS_PRESENT` 里、AS 必须在 MMU 里已分配、
      head 非 0 且 64 字节对齐；`JS_COMMAND_NEXT` 读回非 0 说明还有命令没被
      取走，返回 `Busy`（源里是 `WARN_ON`）。任何一次写失败都用
      `JS_COMMAND_NEXT = NOP`（`panfrost_job.c:549` 的取消命令）收尾并返回原
      错误，且不推进链序号，重试仍是同一条链。写入在
      `NATIVE_MALI_M6_WRITES`（默认 0）之后。
      `GPU_LATEST_FLUSH_ID` 的读取放在 `NativeMaliGpuCore::latestFlushId()`：
      源里 GPU 不在用就返回 0，这里改成返回门禁错误——0 本身是合法 flush id，
      调用方分不出「没刷过」和「没读到」。
- [x] **完成判定：轮询 `JOB_INT_RAWSTAT`**（2026-10-27 选定）。中断那条要先有
      `NativeMaliIrq`（`JOB` 线是 DTB 的 `0x8` 电平低，见 `OPEN_QUESTIONS` 第 2
      条）以及 workloop/命令门，且没有任何一部分能在 host 上验证；轮询是能先在
      假寄存器上证明、再上机证明的最小一步。因此 `JOB_INT_MASK` **不解掩**、
      驱动不认领任何中断。
      `waitForCompletion(js)` 读 `JOB_INT_RAWSTAT`，看 `BIT(js)`（DONE、
      `JOB_INT_MASK_DONE`）和 `BIT(js + 16)`（ERR、`JOB_INT_MASK_ERR`，
      `panfrost_regs.h:241-242`），每 10 µs 一次、默认 100 ms 预算。
      - DONE：写 `JOB_INT_CLEAR = BIT(js)`，只清本槽位。源里
        `panfrost_jm_handle_irq()`（`panfrost_job.c:520-571`）清的是整字，但它
        在同一个写里采样 `JOB_INT_JS_STATE`，且每个槽位排两个 job，必须靠
        JS_STATE 才能区分；这里一个槽位只有一个在飞的 job，DONE/ERR 本身就
        唯一，完成判定里读 JS_STATE 只会引入它自己的竞争（`quiesce()` 在 reset
        路径上读它，那是另一件事）。清别人的位则会偷走那个槽位的
        完成事件。
      - ERR：先读 `JS_STATUS(js)`（`panfrost_job_handle_err()`，
        `panfrost_job.c:454-470`，低 8 位是 `NativeMaliFault` 那张表），再写
        `JS_COMMAND_NEXT = NOP` 取消已排队的下一个命令（`panfrost_job.c:544-550`），
        再清位，然后把该槽位**锁为 faulted**：`submit()` 返回 `NotReady`，
        直到调用方 `clearFault(js)`。源里 `STOPPED`（0x03）是可恢复的 stop，
        会更新 `job->jc` 后重发；这里没有 job 对象，所以 `waitForCompletion()`
        只如实报告异常码并锁住，恢复走下面显式的复位路径，不在等待里假装恢复。
      - 超时：不写任何寄存器、不清 outstanding，可以重试；超时不是完成。
      - 清理写失败：错误原样返回，outstanding 保持，重试会重复同样的清理
        （幂等）；`completion` 里已填的 `rawStatus`/`jsStatus` 仍是真的。
      - 一次只有一个 job：`submit()` 在 outstanding 时返回 `Busy`。这是本驱动
        的限制，不是硬件限制（源里每槽两个）。
      - 提交前还会读一次 `JOB_INT_RAWSTAT`，若本槽位已有 DONE/ERR 就清掉：
        复位可能留下陈旧的 job 事件（`panfrost_jm_reset_interrupts()`，
        `panfrost_job.c:435-438` 专门清全部），而我们的 `softReset()` 只知道
        `GPU_INT_CLEAR`。不清的话下一次等待会立刻“完成”。
      - 假寄存器模型只加了两条真硬件行为：`JS_COMMAND_NEXT` 写入后读回 0，
        `JOB_INT_CLEAR` 对 `JOB_INT_RAWSTAT` 是写 1 清零。
      上机要确认的一件事记在 `OPEN_QUESTIONS` 第 14 条：`JOB_INT_MASK` 为 0 时
      `RAWSTAT` 是否仍然记账（寄存器命名与源里的用法都指向“是”，但没有一行
      源能证明）。
- [x] **软件 fence**：`NativeMaliFence{js, seqno}` 是源里
      `fence->seqno = ++queue[js].emit_seqno`（`panfrost_job.c:106`）的等价物；
      `submit()` 成功时通过 out 参数交出去（失败先清零，不留假 fence），
      `latestSeqno()`/`completedSeqno()`/`fenceSignaled()` 让调用方事后问
      「那条 job 还在飞吗」而不用等。**signaled 只表示被 `waitForCompletion()`
      收下了**（DONE 或 ERR 都算），成功与失败看那次 reaper 拿到的
      `NativeMaliJobCompletion`——源里 fence 的 error 也是这么分开的。
      超时不算完成，所以 fence 保持未 signaled、job 保持 outstanding，重试后
      才 signaled。`clearFault()` 与 GPU 复位都不重置序号（源里也不重置
      `emit_seqno`），因为序号只用于比较。`tests/test_job_slot.cpp` 锁住这些。
- [x] **复位路径的软件部分**：`NativeMaliJobSlot::quiesce()` 按
      `panfrost_reset()`（`panfrost_job.c:697-709`）先给每个 present 槽写
      `JS_COMMAND_NEXT = NOP`、`JS_COMMAND = SOFT_STOP`，再每 10 µs 读
      `JOB_INT_JS_STATE`、用 `JOB_INT_RAWSTAT` 的 DONE/ERR 把已经结束的槽
      从掩码里去掉（`panfrost_active_slots()`，`:637-656`），预算 10 ms。
      超时返回 `Timeout`（源里只 log "Soft-stop failed" 再照常复位），
      寄存器错误和控制门错误原样返回。
      `resetInterrupts()` 按
      `panfrost_jm_reset_interrupts()`（`panfrost_job.c:435-438`）一次写
      `JOB_INT_CLEAR = ALL_JS_INT_MASK`。掩码按 `panfrost_device.h:29-31` 的
      `GENMASK(18,16) | GENMASK(2,0)` 用 `NUM_JOB_SLOTS = 3`
      （`panfrost_job.h:48`）算成 `0x70007`；它照常过 prepared / M6 开关 /
      `gpuReadable()`，写失败原样返回，重复调用幂等。这对应源里
      「复位可能留下陈旧的 job 事件」那一步。
      `abandonOutstanding()` 是 `panfrost_reset()` 里调度器那一半
      （`drm_sched_stop()` + 给在飞的 fence 打错误）在没有 scheduler/job 对象
      时的等价物：纯状态丢弃，清 outstanding 与 fault 锁存，**保留**
      `emitSeqno_`/`completedSeqno_`（源里也不回退，序号只用于比较），零寄存器、
      幂等。被丢弃的 fence 不会变成 signaled——它没有被完成收下。
      `NativeMaliUserClient::noteReset()` 在加 resets 的同时记下每个槽当时在飞
      的那一个 seqno（`latestSeqno > completedSeqno` 且属于本客户端），
      `waitFence` 对正好这条 fence 返回 `IOError`（源里被 reset 的 fence 就是
      错误 fence），而不是永远超时；reset 不是 job fault，所以 `faults` 不变。
      两者调用顺序无关，因为 `abandonOutstanding()` 保留两个序号；之后没杀掉
      任何 job 的 reset 不会清掉旧的死标记（`completedSeqno` 只前进，清掉就会
      把已销毁的 fence 误判成普通完成）。
      编排在 `NativeMaliService::reset()`（M7）：`quiesce → softReset →
      powerOn → resetAllocation → resetInterrupts → abandonOutstanding →
      noteReset → 重新 enable 每个客户端的 AS`；soft-stop 超时只计数，不阻止
      复位。
- [ ] **完成通知（不是轮询）**：`waitForCompletion()` 仍然是同步阻塞的；要把
      fence 的「signaled」变成能唤醒等待者的通知，需要 `NativeMaliIrq`
      （JOB 线，DTB 的 `0x8` = 电平低）与第 14 条的实测答案，所以放在 M7 之后。
      第一个真机证明仍然是 WRITE_VALUE。

## M7 UserClient 与 ABI

- [x] **`NativeMaliABI.h`**：固定大小的记录 + 方法号 + `static_assert` 布局，
      形状照 `NativeAdrenoABI.h`（小端、定长、未知版本/方法/标志一律拒绝、
      数组长在记录里）。M4/M6 定稿之后才写，字段就按驱动真能兑现的能力来：
      Query/GetDeviceInfo（读 M1/M3 的读数）、BoAlloc/BoMap/BoFree/BoCpuMap/
      BoCpuUnmap（M5）、Submit/WaitFence（M6 的 chain 地址 + 软件 fence）、
      GetStatus。**刻意不放**的字段与理由都写在头文件里：
      - 没有 CreateQueue/CloseQueue：一个 queue 就是一个地址空间，这台机器的
        `GPU_AS_PRESENT` 只有 3 位（`docs/DEVICE.md`），每个客户端一个 AS 才
        不会互相饿死，所以 Submit 不带 queue 号。
      - 没有槽位字段：3 个 job slot 是共享硬件，由 service 挑一个，挑不到就
        `Busy`（源里也是调度器挑，`panfrost_job.c`）。
      - 没有 BoImport：把调用方自己的页钉住需要一个能描述它们的 memory source，
        host 测试用的 `NativeMaliMemorySource` 还做不到（第 5 条）。
      - 没有 requirements/affinity：本驱动按 `features.shader_present` 全核启动
        （`panfrost_job.c:135-151`），放一个不遵守的字段等于撒谎。
      - 没有 flush id：`GPU_LATEST_FLUSH_ID` 由 service 自己读。
      常数都从代码里推出来：页 4 KiB（`NativeMaliVmGranule`）、单个 BO 上限
      128 MiB（`NativeMaliVmMaxTables` 64 × 512 项 × 4 KiB = 2^27）、chain
      64 字节对齐（`NativeMaliJobHeadAlignment`）。
      fence 对用户态不透明：低 32 位是 `NativeMaliJobSlot` 的 seqno、高 32 位是
      槽位，客户端只能原样传回，不能解码。
- [x] **`NativeMaliAbiValidate`**：一条记录自己就能判定的检查（服务还需要查自己
      的表才能判的，比如 handle 是否存在、fence 是否属于这个客户端、IOVA 是否
      已被占用，不在这里）。畸形参数是 `BadArgument`，未知版本/方法/标志位是
      `Unsupported`（那是更新的客户端，不是坏请求），格式合法但要不到的是
      `NoResources`。`tests/test_abi.cpp` 逐项锁住：尺寸不是页整数倍、超过上限、
      IOVA 未对齐/为 0/越界/相加回绕、没有读权限、未知权限位、chain 未按 64 字节
      对齐、没有 fence 却有 timeout、seqno 为 0 的 fence，以及 fence 编解码的
      往返。
- [x] **`NativeMaliBoTable`（BO 这一半的状态机）**：`NativeMaliAbiValidate`
      之上的表，固定 64 个槽位、`NativeMaliHandleIndex` 做 handle→槽位。
      `open()` 绑定这个客户端唯一的一个 AS（`NativeMaliVm` + `NativeMaliMmu` +
      槽号），并拒绝未 present/未分配的空闲槽；allocate/map/free 全部走真实的
      `NativeMaliMemory` 与真实页表。几条来自源与先例的规则：
      - handle 单调递增、永不为 0（`NativeAdrenoUserClient.h:29-46` 的形状），
        所以释放后留下的旧 handle 不会指向新对象。
      - `freeBo()` 先 `unmap()` 再 `release()`（`panfrost_mmu.c:500-524`）；
        unmap 失败时对象保持 live + mapped，重试只重发那次 flush，页不会先被
        收走；`NativeMaliMemory::release()` 自己也会拒绝 mapped 对象。
      - 同一对象第二次 map 是 `Busy`；两个对象范围重叠在写第一片叶之前就拒绝
        （不是先映射再回滚）；`map` 交给页表的权限是 `Read|Cached`（`Write`
        只在调用方要时加），也就是 `panfrost_mmu.c:466-475` 的
        `IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE`，只读是唯一一个源里没有的
        子集，编码器能表达。
      - CPU 映射是 token（槽位 + 1）而不是第二个 handle；`cpuMap` 两次是
        `Busy`，`cpuUnmap` 的 token/handle 任一不符都拒；有 token 的 BO 不能
        free（`Busy`），因为包装层还映射着它。
      - `releaseAll()` 先丢 token（连接都要关了，与
        `NativeAdrenoUserClient::releaseAll()` 一致）再按槽位顺序 free，第一个
        失败就停、其余保持 live；`reset()` 只用于 AS 已经拆掉之后，只放 CPU 侧
        资源、不碰寄存器。
      `tests/test_bo_table.cpp` 用假寄存器跑真实页表，锁住以上全部与 table 满、
      源分配失败、flush 失败后重试。
- [x] **`NativeMaliUserClient`（BO 表之上的状态机）**：纯逻辑，不依赖 XNU。
      - `open()` 绑定这个客户端的 AS、BO 源、共享的 `NativeMaliJobSlot` /
        `NativeMaliGpuCore` 和 M3 的 `NativeMaliGpuObservation`；Query/
        GetDeviceInfo 报的是实际建出来的页表几何（`NativeMaliVm::info()`）、
        probe 读数和 non-coherent 标志。
      - Submit：chain head 必须落在本客户端一个 live mapping 内
        （`NativeMaliBoTable::covers()`，不是信任调用方给的 GPU 地址）；有
        `wait_fence` 先等它；槽位由驱动挑——源里是
        `panfrost_job_get_slot()` 按 requirements 挑（FS→JS0，其余 JS1，
        compute→JS2），本 ABI 故意没有 requirements，所以诚实策略是「从上次
        用过的下一个开始，第一个 present、非 faulted、非 outstanding 的槽」，
        一个槽同时只跑一个 job（`panfrost_job.c:113-138`）；flush id 每次提交
        从 `GPU_LATEST_FLUSH_ID` 读（`panfrost_gpu.c:561-573`），不让调用方选。
      - fence 归属：每个槽记下本客户端拿到的最高 seqno。更高的一律拒绝（那是
        别的客户端更新或还没提交的 job），绝不替别人 reap；更低的、本客户端
        跳过的 seqno 属于同一槽上别人更早的 job，而槽一次只跑一个，所以它必然
        已经完成，`fenceSignaled()` 直接答 true，不碰硬件。这样既不能偷
        completion，也不会把别人的 fault 记到自己头上。
      - 完成是轮询的（M6）：DONE reap、ERR 也 reap 并把槽标 faulted、把
        `GetStatus::faults` 加一（`isFault()`，exception > 0x3F）；timeout 不改
        任何状态、可以重试。`noteReset()` 是 service 在 GPU reset 后调的，
        对应 `GetStatus::resets`。
      - `close()`/`releaseAll()` fail closed：有 live 对象或自己的 job 没 reap
        就拒绝；拆除先等自己的 job（不是自己的槽上 job 不等），先丢 CPU
        token，再按槽位 unmap→release；flush 失败时对象保持 live + mapped，
        重试只重发那一次 flush。与 `NativeAdrenoUserClient::releaseAll()` 的
        顺序一致。
      `tests/test_user_client.cpp` 用真实页表、真实 job slot 和真实 flush id
      读锁住以上全部；12 个 mutation 各自打在不同断言上。
      - `dispatch(method, input, input_size, output, output_size)` 是未来
        `IOUserClient::externalMethod` 的纯半边：每个方法号只对应一个定长输入
        记录和一个定长输出记录（可以没有），尺寸不符是 `BadArgument`，未知
        方法号是 `Unsupported`，只有方法成功才写输出缓冲；不会替调用方判断
        GPU 可不可用，服务层的 `clientUsable()` 门禁在它上面。形状参照
        `NativeAdrenoUserClient::dispatch()`（MIT，见 NOTICE），记录和代码都是
        本仓库自己的。`tests/test_user_client.cpp` 的 dispatch 用例锁住方法表、
        两个方向的尺寸（差一字节、多一字节都算）、空指针、未知方法、坏版本、
        完整 BO/job 生命周期，以及每次拒绝都不改输出。dispatch 这一轮又做 12 个
        mutation（方法表、未知方法、两个方向的尺寸与空指针、GetStatus 漏拷贝、
        Query/Submit 失败后仍拷贝），全部被抓住。
- [ ] IOKit 胶水（`NativeMaliService` 子类、`NativeMaliGpuClient`）：照
      `NativeAdrenoService.cpp` 的角色划分，等 XNU 能力清单。纯的
      `NativeMaliService` 已经能一次做完 M1→M4、每个客户端的 AS 生命周期和
      复位编排，`NativeMaliUserClient::dispatch()` 也已经把记录尺寸、未知方法和
      失败不改输出做完了。`xnu/` 现在有**纸面实现**：窗口适配器、页表/buffer 的
      `IOBufferMemoryDescriptor` 源、缓存维护、`NativeMaliGpuService`/
      `NativeMaliGpuClient`、`Info.plist`。它**没有在 XNU 头文件下编译过**，也没有在真机上跑过；
      每个未验证的假设就地标 `UNVERIFIED(XNU)` / `UNVERIFIED(HW)`，清单和
      「怎样才算验证」写在 `docs/UNVERIFIED.md`。所以这一步仍不算完成，剩下的是
      在 UniXNU-merlinx 里编译、上机、删除对应标记；对接步骤（子模块、ADT 节点、
      `build-arm-driver.py`、`board.toml`、`Info.plist`、验证命令）写在
      `docs/INTEGRATION.md`。

## M8/M9/M10

- [ ] M8 GOP 冒烟：需要能跑起来的机器（你正在解 panic）。
- [ ] M9 Mesa 评估：等内核路径通了再评估工作量。
- [ ] M10 加固：挂起/恢复、空闲关核、CI、许可证逐文件审计。
