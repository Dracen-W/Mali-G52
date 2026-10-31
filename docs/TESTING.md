<!-- SPDX-License-Identifier: GPL-2.0-only -->
# 怎么测试这套代码的逻辑

一句话：**所有不依赖硬件的逻辑都在 host 上可执行、可断言**，一条命令跑完；
涉及 MMIO、中断、时钟、电源的部分，host 只能测「顺序」和「拒绝路径」，
真正的证明要等 M2 之后上机。

## 1. 跑一次

```sh
tests/run.sh          # 编译并运行全部 host 测试
tests/run.sh --san    # 同一批测试，开 AddressSanitizer + UBSan
CXX=clang++ tests/run.sh
```

当前输出（本机实跑）：

```
check  NativeMaliRegs.h
build  test_abi
run    test_abi
PASS: Mali wire ABI layout goldens and the pure user-client checks ...
build  test_bo_table
run    test_bo_table
PASS: BO table open/alloc/map/free/cpuMap/cpuUnmap against the real page tables ...
build  test_board
run    test_board
PASS: board parser over tests/data/mt6768.dtb ...
build  test_bringup
run    test_bringup
PASS: M1 read before write, M2->M3->M4 order across blocks, ...
build  test_fault
run    test_fault
PASS: fault exception/access decode against the pinned panfrost table, ...
build  test_gpu
run    test_gpu
PASS: GPU identity decode (product/revision split, T60x fixup, ...)
build  test_gpu_core
run    test_gpu_core
PASS: GPU feature probe behind the M2 gate, raw feature/present words, ...
build  test_handle_index
run    test_handle_index
PASS: handle index allocate/release, insert/replace, key-0 refusal, ...
build  test_irq
run    test_irq
PASS: GPU/JOB/MMU interrupt status decoding against the pinned handlers, ...
build  test_job
run    test_job
PASS: v7 job header golden words, WRITE_VALUE payload, ...
build  test_job_slot
run    test_job_slot
PASS: JS slot submit order and golden JS_CONFIG, per-slot chain-flag parity, ...
build  test_memory
run    test_memory
PASS: buffer spans, allocation/release, span validation, ...
build  test_mmu
run    test_mmu
PASS: MMU AS allocation, lock-region encoding, ready polling, ...
build  test_pmic
run    test_pmic
PASS: WACS2 read/write/FSM/timeout/stale-valid, MT6358 selector arithmetic, ...
build  test_power
run    test_power
PASS: read-only probe, voltage->clock->power->gate bring-up, ...
build  test_regs
run    test_regs
PASS: generated register map matches the pinned panfrost_regs.h ...
build  test_user_client
run    test_user_client
PASS: user client open/Query/GetDeviceInfo, BO forwarding, chain ...
build  test_vm
run    test_vm
PASS: Mali LPAE geometry (ias 32/39/40/48), descriptor golden words, ...
all host tests passed
```

`tests/run.sh` 把仓库根目录的 `NativeMali*.cpp` 全部链进去，所以新增模块不用改脚本。
内存相关的三个测试（`test_memory`、`test_bo_table`、`test_user_client`）共用的
host 假实现放在 `tests/FakeMaliMemory.h`：`TestPages`（页表页）、
`FakeMemorySource`（span 表，含各种违约模式）和 `FakeCache`。
每个测试是一个独立可执行文件，放在 `build/host/`，也可以直接单独跑：

```sh
./build/host/test_vm
./build/host/test_board tests/data/mt6768.dtb
```

测试构建额外定义 `NATIVE_MALI_M2_WRITES=1`、`NATIVE_MALI_M3_WRITES=1`、
`NATIVE_MALI_M4_WRITES=1` 和 `NATIVE_MALI_M6_WRITES=1`。这是**唯一**打开这些
写入路径的构建；驱动本身不定义它们，所以生产构建里 `setFixedVoltage()`/
`selectMfgClockSource()`/`powerOnDomains()`/`enableMfgClockGate()`/`bringUp()`、
`NativeMaliGpuCore::softReset()`、`NativeMaliMmu::enable()`/`disable()` /
`flushRange()` 以及 `NativeMaliJobSlot::submit()` 全部返回
`kIOReturnUnsupported`（`tests/run.sh` 的注释和四个头文件都写了这一点）。
`probe()`、`readFault()`、`latestFlushId()` 不受影响，它们本来就是上机第一步
的只读路径。M5 没有自己的开关：`NativeMaliMemory::map()`/`unmap()` 的最后一步是
M4 的 `flushRange()`，所以 M4 写入关闭时它们也以原错误失败，而不是映射完却不
发布。

## 2. 现在测了什么，断言里的数从哪来

测试不是「跑一遍没崩」，每条断言都有来源：

### `tests/test_board.cpp` —— 设备树解析

- 输入是**真机 DTB 的真实字节**（`tests/data/mt6768.dtb`，sha256 在
  `tests/data/SHA256SUMS`），不是手写的假数据。
- `reg` = `0x13040000/0x4000`、五个中断 274..278、八个命名时钟及其
  phandle（topckgen `0x14` / mfg_cfg `0x60` / scpsys `0x27`）全部从 DTB 读出后比对。
- 中断按 `interrupt-names` 取名，故意再用一份**乱序**的属性验证「位置无关」。
- `checkTriggers()` 断言五个源都是 SPI + 电平低（0x8），并验证如果把其中一个改成
  高电平、边沿、PPI 或 SPI 范围外，都会被拒绝。
- 40 多条畸形/越界用例：截断的属性、重复时钟名、未知中断名、容量不足、
  地址回绕、非零写入……每条都断言 **失败时不写调用方缓冲区**。

### `tests/test_regs.cpp` —— 生成的寄存器表

- `NativeMaliRegs.h` 是 `tools/generate-regs.sh` 从钉死的 `panfrost_regs.h` **逐行复制**的
  产物（源文件哈希对不上 `third_party/PINNED.md` 就拒绝生成）。
- `tests/run.sh` 每次先重新生成到 `build/` 再 `cmp`，所以「表是旧的」会先于任何测试失败。
- `test_regs.cpp` 锁住驱动真正用到的值：`GPU_ID 0x00`、`GPU_INT_CLEAR 0x24`、
  `GPU_CMD 0x30`、各 present/ready/pwron 位图、`JS_HEAD_LO(n) = 0x1800 + n*0x80`、
  `MMU_AS(0) = 0x2400`、`AS_COMMAND(3) = 0x24d8`、`AS_TRANSTAB_LPAE_*` 等，
  并交叉验证 `NativeMaliVm::encodeTable()` 与 `AS_TRANSTAB_LPAE_*` 位义一致。
- 这里也能抓到 `BIT()` 宽度错误：`AS_TRANSCFG_XREADABLE` 是 `BIT(36)`，
  如果 shim 把 `BIT` 定义成 32 位，断言立刻失败。

### `tests/test_abi.cpp` —— 线 ABI 的布局与纯校验（无 IOKit、无寄存器）

- 布局：`NativeMaliABI.h` 的 `static_assert` 已经钉死每个记录的大小，这里再用
  `offsetof` 钉死字段顺序（Query 的 flags/max_bo_bytes、DeviceInfo 的 flags/
  va_start/va_size、BoMap 的 iova/permissions/reserved、Submit 的 wait_fence/
  timeout/flags/reserved），中间插字段就会失败。
- 常数与来源一致：页 4 KiB、chain 64 字节对齐、单个 BO 上限
  `64 × 512 × 4096 = 2^27`、权限位与 `NativeMaliDeviceNonCoherent`。
- 拒绝路径：版本 0/2/`0xFFFFFFFF` 与未知方法号是 `Unsupported`；BO 大小不是页
  整数倍或为 0 是 `BadArgument`，超过上限是 `NoResources`；IOVA 为 0、未对齐、
  越界或 `iova + bytes` 回绕、没有读权限、权限里有未知位、`reserved` 非 0 各自
  被拒；chain 为 0 或不是 64 字节对齐被拒；没有 fence 却给 timeout 被拒；seqno
  为 0 的 fence（包括 `slot << 32` 这种）在 Submit 与 WaitFence 都被拒。
- fence 编码：`packFence`/`fenceSlot`/`fenceSeqno` 在三个槽位与 seqno
  `1/2/0xFFFFFFFF` 上往返；`packFence(0, 0) == 0` 正是「0 表示没有 fence」的
  依据。
- 这里**不**测 handle 是否存在、fence 是否属于这个客户端、IOVA 是否被占用——
  那需要客户端的表，属于 `NativeMaliBoTable`（下面）。

### `tests/test_bo_table.cpp` —— 客户端的 BO 表（真实页表 + 假寄存器）

- `open()`：未 open 时 alloc/map/free/releaseAll 都是 `NotReady` 且 reply 被
  清零；source/vm/mmu 为空、槽号越界、non-coherent 却没有 cache maintainer、
  以及 **MMU 里没 present/没分配**的槽都拒绝；成功后再 open 是 `Busy`。
- alloc：0/非页整数倍的尺寸是 `BadArgument` 且不调 source；超过 `max_bo_bytes`
  是 `NoResources`；source 自己失败（`NoMemory`）原样返回、不占槽位；handle
  永不为 0 且单调（free 之后新 handle 更大，旧 handle 查不到）；表满第 65 个是
  `NoResources` 且不再向 source 要内存。
- map：未知 handle、iova 0/未对齐/越界、没有读权限、未知权限位各自拒绝，且
  **零寄存器写**；成功时两段 span 各写自己的物理地址、整段一次 flush（4 次
  LOCK/FLUSH_PT 写），叶子的权限位是 `Read|Write|Cached`；只读映射的叶子与
  可写叶子不同；同一对象第二次 map 是 `Busy` 且零写；重叠在写第一片叶前就
  `BadArgument`（包括“起点空闲、第二页撞上已有映射”的情况，此时起始页仍然
  是 0）；换到空闲地址重试成功。
- free：未知/0 handle 拒绝；未映射的对象只 release、不写寄存器；映射中的对象
  先清叶、再 flush 一次、最后 release；flush 失败时返回原错误、对象保持
  live + mapped、页没有 release、叶子已经清零，修好之后重试成功；带 CPU token
  的对象 free 是 `Busy` 且页没有 release。
- CPU token：`token != 0`、`bytes` 是对象大小；`memoryForToken` 只在 token 被
  持有时返回对象；第二次 `cpuMap` 是 `Busy`；token 不符、reserved 非 0、
  已 unmap 之后再 unmap 都拒绝；stale token 不能指向后来占用同一槽位的新对象
  （handle 必须也对上）。
- 拆除：`releaseAll()` 先丢 token 再按槽位释放（映射中的对象也清叶 + flush），
  全部归还 source；中途 flush 失败时停下、已处理的释放掉、剩下的保持 live，
  重试成功；`reset()`（AS 已经拆掉之后）对仍映射的对象也只放 CPU 侧资源、
  零寄存器写，地址空间里的叶子保持原样，之后 alloc 是 `NotReady`。

### `tests/test_user_client.cpp` —— 客户端状态机（真实页表 / job slot + 假寄存器）

- 未 open 时每个入口都是 `NotReady` 且 reply 被清零（alloc/map/cpuMap/Query/
  DeviceInfo/Submit/Status）；`open()` 对 null vm/jobs/gpu、null source、非
  coherent 却没有 maintainer、MMU 没分配的 AS 各自拒绝，成功后再 open 是
  `Busy`。
- Query：版本不符/未知 flag 位是 `Unsupported`，null reply 是 `BadArgument`；
  成功时 `abi_version`、`input_bits=48`、`output_bits=40`（来自实际页表几何）、
  `max_bo_bytes` 与 open 时给的上限一致。DeviceInfo 逐字段核对 M3 probe 输出、
  non-coherent 标志、`va_start=0`、`va_size=1<<48`；coherent 客户端不需要
  maintainer 且标志为 0。
- BO 转发：alloc/map/cpuMap/cpuUnmap/free 与 BO 表同一套语义，token 只在持有
  期间能查到对象，全部归还 source。
- Submit 拒绝：chain 未 64 字节对齐、为 0、在映射前/后各 64 字节、未知 flag、
  reserved 非 0、没有 fence 却给 timeout、null reply 全部被拒，且**零寄存器
  写**；映射最后 64 字节里的 chain 成功（并在成功路径里写到
  `JS_FLUSH_ID_NEXT` 的正是 open 时放在 `GPU_LATEST_FLUSH_ID` 的值）。
- 槽位选择：三个槽按 0→1→2 轮转、seqno 各自从 1 开始；槽 0 被 ERR 标 faulted
  后跳过它，绕回来时落在槽 1 而不是报错；每个 job 的 `JS_FLUSH_ID_NEXT` 都来自
  提交当次读到的 flush id。
- fence 归属：另一个客户端直接 `jobs.submit()` 拿到的 fence（即使它已经完成）
  在 WaitFence 和作为依赖时都是 `BadArgument`，且没有 reap、没有写寄存器；本
  客户端没到过的 seqno 同样拒绝；本客户端自己的 fence 可以用，槽上更高的
  seqno 不行；0 与 seqno 0 是格式错误。
- 依赖等待：已 latch DONE 的依赖会被 reap，新 job 落在下一个槽；未完成的依赖
  在 0 预算下是 `Timeout`、不启动新 job（没有新的 `JS_HEAD_NEXT` 写）；完成
  后重试成功；依赖以 ERR 结束时返回 `IOError`、faults 加一、不启动新 job。
- 完成：0 预算轮询一次是 `Timeout` 且 job 仍 outstanding，可重试；DONE 后
  `Success`，同一个 fence 再问一次也 `Success`（不再轮询）；ERR（exception
  0x40）返回 `IOError`、faults=1、槽 faulted；非 fault 的 ERR（0x01）也是
  `IOError` 但 faults 不变；`noteReset()` 让 resets 加一。
- 复位：提交后 `noteReset()` 把当时在飞的 seqno 记下来，`waitFence` 对这条
  fence 返回 `IOError`（重复等待仍是 `IOError`，不读不写），`faults` 不变、
  `resets` 加一；作为 Submit 的依赖同样是 `IOError` 且不启动新 job。
  `abandonOutstanding()` 之后槽可复用（三个槽轮转、序号继续），而
  `releaseAll()` 不会等一个已被 reset 销毁的 job。先 abandon 再 noteReset 也
  能认出这条 fence；已经 reap 的 job 在 reset 后仍然是 `Success`（不是错误
  fence），而一次什么都没杀的 reset 也不会擦掉更早的死标记——被销毁的 fence
  始终是 `IOError`，不会因为序号推进而被当成普通已完成。
- close/teardown：有 live 对象或自己的 job 没 reap 时 `close()` 是 `Busy`，
  两者都清掉后成功，之后所有入口回到 `NotReady`；重新 open 从 0 计数。
  `releaseAll(0)` 在 job 未完成时是 `Timeout`、什么都不释放；完成后先等 job，
  再 unmap→release；flush 失败时第一个对象保持 live+mapped（叶子已清、第二个
  仍未动），修好后重试成功；共享槽上别的客户端的 job 不会让本客户端等待。
- dispatch：`inputSize()`/`outputSize()` 对 10 个方法逐个断言等于对应记录，
  0 和 `NativeMaliMethodLast + 1` 都是 `kUnsupportedSize`；未知方法号是
  `Unsupported`，输入/输出尺寸差一字节或多一字节都是 `BadArgument`，必需记录
  传空也是 `BadArgument`，三种情况输出缓冲一个字节都不变。Query 的好版本回真实
  几何、`max_bo_bytes` 是 open 时给的 ABI 上限，坏版本 `Unsupported` 且回复保持
  原样；GetDeviceInfo 与直接调用逐字节相同；BoAlloc→BoMap→BoCpuMap→
  BoCpuUnmap→Submit→WaitFence→GetStatus→BoFree 全部走 dispatch；越界的 chain
  在写寄存器前拒绝且 reply 不变；GetStatus 在 `noteReset()` 之后回 `resets=1`
  且与直接调用逐字节相同；未 open 的客户端返回 `NotReady` 且不碰输出。

### `tests/test_service.cpp` —— 每个客户端一个 AS 与复位编排（全部假块，真实下层）

这一层不引入任何新寄存器：它把 `NativeMaliBringup`、共享的
`NativeMaliJobSlot`、真实的 `NativeMaliMmu` 编程、真实的 `NativeMaliVm` 页表
和真实的 `NativeMaliUserClient` 提交串起来，用同一个假 GPU 窗口。假块为此补了
三条 job-manager 行为（`JS_COMMAND_NEXT` 读回 0、`JOB_INT_CLEAR` 写 1 清
`JOB_INT_RAWSTAT`、`JOB_INT_JS_STATE` 跟着 START/SOFT_STOP/复位走），都是真
硬件行为，不是放宽。

- **start/stop 门禁**：没 prepare 就 start 是 `NotReady`；空块是
  `BadArgument`；start 之后 `jobs().present() == 0x5`、
  `shaderPresent() == 0xF`、观察值来自 M3 probe、MMU `allocatedMask()` 为 0；
  第二次 start 是 `Busy` 且零写；stop 可重复。
- **开客户端**：第一个拿 AS0、第二个 AS1、第三个 AS2（`GPU_AS_PRESENT` 三位），
  第四个是 `NoResources` 且零写；TRANSTAB 在 UPDATE 之前写入，且是各自 VM 的
  值；同一个客户端开两次是 `Busy`；null client/vm/表源/buffer 源都是
  `BadArgument`；未 start 是 `NotReady`。
- **关闭**：`releaseAll()` 先跑（有 BO 时 source 的 release 计数加一），再
  disable → release → `vm.reset()`，分配位和页表都归零；再关一次是
  `BadArgument`。
- **开的回滚**：enable 第一条命令就失败、TRANSTAB 之后失败、页表源分配失败三种
  情况都断言 AS 号已还、表已退、client 没 open、`teardownError` 是回滚自己的
  错误；第三种还证明失败发生在 take 和 program 之间也不会漏号。第四种是非一致
  客户端却没给 maintainer：BO 表在 `client->open()` 里拒绝，回滚走
  `rollbackOpen()`（disable 的 TRANSTAB 清零恰好一次 → release → `vm.reset()`），
  所以这条路径也有覆盖。
- **关不掉时**：disable 失败 → `closeClient` 返回原错误、client 已
  `releaseAll`（不再 open）、但仍注册、AS 仍分配、VM 仍 prepared，`stop()`
  是 `Busy` 零写；硬件恢复后重试成功。
- **块不可读时关闭**：先让 SPM 不再报告 MFG 域（`gpuReadable()` 失败），reset
  在 `softReset()` 的前置检查就返回 `NotReady` 且零写；此时 `closeClient()`
  不能靠 flush/disable，走 `abandon()`（BO 直接还回 source）→ release 号 →
  `vm.reset()` → 注销，GPU 写入数不变。域恢复后 `stop()` 正常完成。这条路径
  对应 `panfrost_mmu_release_ctx()` 的 `pm_runtime_active()` 守卫
  （`panfrost_mmu.c:731-732`）。
- **有 job 在飞时关闭**：`releaseAll()` 等不到完成 → `Timeout`，client 仍 open、
  AS 仍分配、`stop()` `Busy` 零写；latch DONE 后重试一次就完成 reap+拆除。
- **stop 的 job 检查独立于 AS 检查**：故意先 `release()` 掉还在跑 job 的 AS
  （owner bug），`stop()` 仍是 `Busy` 且 GPU/SPM 零写、块保持上电；latch DONE
  并 reap 之后 `stop()` 才成功。
- **复位**：真实提交一条 chain（映射过的 BO、chain head 在映射内、flush id 来自
  `GPU_LATEST_FLUSH_ID`）后 reset：先对两个 present 槽（`GPU_JS_PRESENT = 0x5`）
  写 `JS_COMMAND_NEXT = NOP` + `JS_COMMAND = SOFT_STOP`，不碰不存在的槽 1，
  且这四次写都在 `GPU_CMD` 软复位之前；然后 `GPU_CMD` 软复位加一、
  `JOB_INT_CLEAR = 0x70007` 恰好一次、`RAWSTAT` 清零、槽不再 outstanding、
  `softStopTimeouts()` 仍是 0、两个客户端的 TRANSTAB 都重新写入、AS 号仍是
  0/1、`clientUsable()` 为真；在飞 fence 变 `IOError`、`resets == 1`、
  `faults == 0`（第二个客户端也收到 reset 计数）；之后提交落在轮转的下一个槽
  （槽 2）并从 1 重新计数。
- **soft-stop 超时与中止**：fake 的 `softStopStuck` 让槽的 `JS_STATE` 位不被
  SOFT_STOP 清掉 → quiesce 用完 10 ms（1000 次 10 µs 睡）返回 `Timeout`，
  reset 仍成功、`softStopTimeouts() == 1`，随后的软复位把 `JS_STATE` 清掉；
  再来一次空闲 reset 不增加计数。quiesce 的 `JS_COMMAND` 写失败 → reset 返回
  `IOError`、`GPU_CMD` 没被写、`clientUsable()` 为假、下一次 reset 是
  `NotReady`；close/stop/start 之后计数清零。
- **复位失败**：`JOB_INT_CLEAR` 写失败 → reset 返回 `IOError`，但 job 已被销毁
  （outstanding 清零、fence 是 `IOError`、resets 加一），`clientUsable()` 为假、
  新客户端 `NotReady`；close/stop/start 之后恢复正常。某个客户端的 AS 重新
  enable 失败 → reset 返回 `IOError`、所有客户端都不可用、AS 号仍被占着
  （`stop()` 会拒绝）；close 之后 stop/start 正常。
- **无客户端的复位**：仍然软复位、清状态字，不写 TRANSTAB。

### `tests/test_fault.cpp` —— 故障解码（纯逻辑）

- `exceptionName()` 对着 `panfrost_device.h:258-330` 的码值和
  `panfrost_device.c:425-500` 的表逐项比对：66 个有名码逐个断言精确字符串，
  0..255 全扫一遍确认无名码（以及 0x100、`0xFFFFFFFF`）都返回
  `"Unknown exception type"`，并单独点名 0x07/0x09/0x45/0x7E/0x8B/0xC5/0xCF/
  0xD4/0xDF/0xF0 这些真实空档。
- `isFault()` 的边界是 `MAX_NON_FAULT = 0x3F`：0x08/0x3F 不算故障，0x40/0xFF
  算；`accessName()` 的四种取值，以及高位不影响 access、只取 bit9:8。
- GPU 记录：只取 status 的低 8 位作 exception，地址是 `HI << 32 | LO`
  （`0x1_FFFFF000` 黄金值）。
- MMU 记录：`(status >> 8) & 0x3` 为 access、`status >> 16` 为 source id、
  bit10 为 decoder fault、`(exception & 0xF8) == 0xC0` 为 page fault
  （0xC0..0xC7 全中，0xBF/0xC8/0x40 不中）。
- 解码器本身不碰寄存器；两个读路径在下面两个测试里，配合假寄存器验证。

### `tests/test_irq.cpp` —— 三个中断状态字的解码（纯逻辑）

- GPU：`GPU_IRQ_MASK_ERROR` 就是 `FAULT | MULTIPLE_FAULT`；七个已知源逐个单独
  置位，断言只有对应的 accessor 为真、`error()` 只在故障对里为真、`unknown`
  为零；全置位是 `0x00030781` 且七个 accessor 全真。`MASK_ALL` 之外的位
  （bit1/bit2/bit11/bit31）进 `unknown` 并且不影响访问器——源里会把这些位照原样
  写回 `GPU_INT_CLEAR`，所以不能当成「什么都没发生」。
- JOB：`MK_JS_MASK(js)` 是 `0x10001 << js`；`decodeJobs(word, 3)` 把低半的 DONE
  与高半的 ERR 都折成 `BIT(js)`，`MK_JS_MASK(1)` 同时点亮两张表，`0x00070007`
  点亮三个 slot。硬件没有的 slot（bit3/bit19/bit31）与 `slots = 0`/`17` 都进
  `unknown`，`doneOn(32)` 不是未定义行为而是假。
- MMU：`mmuClearMask(2) == 0x40004`、`(15) == 0x80008000`、`(16) == 0`（只有 16 个
  AS）；低半单独置位是 fault，高半单独置位只算 `second`，两半一起置位两者都为真；
  `GPU_AS_PRESENT` 之外的 AS（包括只在高半出现的那种）进 `absent`，不进 fault；
  并且断言 `fault | second` 正好等于源里 `ffs(status | (status >> 16))` 走的那
  个集合。
- 这个模块只解码：没有一次寄存器访问，也没有一次写。

### `tests/test_gpu.cpp` —— GPU_ID 解码

- 产品号/修订号拆分、`major/minor/status` 位段、T60x 的 `0x6956 → 0x0600` 修正、
  产品号掩码（`0x7102` 与 `0x7002` 等价）、Bifrost 型号表（g71/g72/g51/g76/g52/g31/g57）。
- 断言「修订号没有对应条目 ≠ 不支持的型号」（panfrost 只用它补 issue 位），
  以及未知产品号会被拒绝但字段仍然填好，供上层记录日志。

### `tests/test_gpu_core.cpp` —— 特性探测、复位与上电（假 GPU 寄存器）

- 先测门：域没全开、topckgen mux gate 没开、mfgcfg `BG3D` 没开时，`probe()`
  返回 `kIOReturnNotReady`，且 `gpu.readCount == 0`——一个 GPU 寄存器都没读。
  三种缺失分别测，另外测两个状态寄存器不一致和「父时钟不是 clk26m 但 mux 已开」
  仍然允许读。
- 再测管线：`GPU_ID`、L2/CORE/TILER/MEM/MMU/THREAD 特性字、thread 上限、
  AS/JS present、每个 present slot 的 `JS_FEATURES`、四组 64 位 present 位图
  （LO | HI << 32）、`THREAD_TLS_ALLOC` 都落到正确的字段；计数用 popcount；
  `ias/oas` 从 `GPU_MMU_FEATURES` 的 [7:0]/[15:8] 取出。
- `GPU_ID = 0x70021000` 走真机的 g52/r1p0 黄金值；未知 id 返回 `Unsupported`
  但原始字仍然填好；读失败原样返回；`js_present` 超过 16 位时拒绝而不是读出
  数组；整个 probe 不产生任何写。
- **特性字是合成的**：这台机器还没有报告过真实特性值，测试明确不为硬件数值
  背书；真实值要等上机 `probe()` 后记进 `docs/DEVICE.md`。
- **复位**：断言 `panfrost_gpu_soft_reset()` 的精确写序
  （`INT_MASK=0` → `INT_CLEAR=RESET_COMPLETED` → `CMD=SOFT_RESET` →
  `INT_CLEAR=GPU_IRQ_MASK_ALL` → `INT_MASK=ERROR|PERFCNT|CLEAN_CACHES`）、
  立即完成时只有一次 `RAWSTAT` 读、软复位轮询间隔 10 µs、hard 回退的写序和
  100 µs 间隔；软/硬两次都超时返回 `kIOReturnTimeout`，且 `INT_MASK` 保持 0、
  不再写收尾值。另测域/门未开时**零写零读**、未 prepare 返回 `NotReady`、
  `INT_MASK`/`INT_CLEAR`/`CMD` 写失败、`RAWSTAT` 读失败和 `delay()` 失败都
  原样返回。`FakeGpu` 只建模这一段握手（写 1 清 `RAWSTAT`、`CMD` 后按
  `pollsBeforeReset` 置位），不假装是完整 GPU。
- **故障读取**：`readFault()` 按 `panfrost_gpu.c:32-35` 的顺序读 status、HI、LO
  （三次读），地址拼成 `0x1_FFFFF000`，exception 取低 8 位；不产生任何写。
  M2 门关闭时 `NotReady` 且零读；读失败保留已经读到的字段（HI 成功、LO 失败时
  status 和 HI 有效）；未 prepare 是 `NotReady`，空指针是 `BadArgument`。
- **flush id**：`latestFlushId()` 只读一次 `GPU_LATEST_FLUSH_ID`、不写任何
  寄存器；M2 门关闭时返回 `NotReady` 且零读（不是源里的 0，因为 0 是合法
  值）；读失败原样返回，输出参数保持 0。
- **上电/下电**：`powerOn()` 的写序严格是 `GPU_JM_CONFIG = 0x000F0000` →
  `L2_PWRON_LO` → `SHADER_PWRON_LO` → `TILER_PWRON_LO`，前两者带上第一组核的
  掩码（`l2_present = 0b11` 时只剩 bit 0，tiler 不动）；`readyPolls = 2` 时三个
  `*_READY_LO` 各读 3 次、各睡
  2 次 10 µs。`powerOff()` 的顺序是 shader → tiler → L2，用完整 present 掩码，
  `transitionPolls = 2` 时 9 次读、4 次 1 µs 睡（L2 不睡）。拒绝路径：没 probe、
  probe 过但没 reset、重新 probe 失败使旧掩码失效、未知 id、M2 门在复位后关闭、
  present 有 bit31 以上（上下电都拒绝）、present 为 0；这些情况都是零写。
  回滚：shader 超时 → L2 关回去；tiler 超时 → shader/L2 仍关；第一个写就失败
  → 仍然发三个 PWROFF；回滚里的写失败不覆盖原始超时；`delay()` 错误原样返回
  而不是当成超时。下电成功后 `powerOn()` 必须先 `softReset()`。
  `GPU_JM_CONFIG` 只出现一次、在第一个 PWRON 之前：G52 的 quirk 只需要
  IDVS group size，理由见 `docs/TODO.md` 的 M3 条目。这一写自己失败时只有它
  自己发生——没有 PWRON，也没有 PWROFF；随后的重试不需要再 `softReset()`。
- 测试夹具里的假寄存器堆是堆上的 `std::vector`：每个块 16 KiB、一个 fixture
  有六七个块，几十个 fixture 被内联进 `main()` 时 ASan 的栈帧会超过 8 MiB。

### `tests/test_vm.cpp` —— ARM_MALI_LPAE 页表

- 几何：`ias` 取 32/39/40/48，断言 `start_level`、`pgd_bits`、`span`
  与 `arm_lpae_alloc_pgtable()` 的算术一致，包括 Mali 那条
  「不足 4 级也补成 4 级、根只留一个有效项」的 quirk。
- 描述符：断言**逐位黄金值**，例如
  `encodeLeaf(0x13040000, RW|Cached) == 0x130402c5`
  （type=1 叶、`[7:6]=11`、`[4:2]=1`、`[9:8]=2` 外共享、无 AF/无 nG），
  以及 `XN = 3<<53`、表项 `type=3`、`paddr_to_iopte()` 对 40 位地址原样携带。
- 寄存器值：`AS_MEMATTR = 0x00888d88`、`AS_TRANSTAB = 根表物理地址 | 3 | 4`
  （coherent 时再 `| 16`）。
- 结构：4 级表在什么情况下复用、什么情况下新建（同 2 MiB 槽 `+0`，
  新 2 MiB 槽 `+1`，新 1 GiB 区 `+2`），以及 64 张表的上限是**报错而不是截断**。
- 拒绝路径：重复映射（`Busy`）、未对齐、跨 `ias`/`oas`、既缓存又 MMIO、
  没读写权限、未 prepare、地址回绕。
- 页表发布：非一致 walker 挂上假 maintainer 后，prepare() 先 clean 一张根表
  （`io-pgtable-arm.c:1263` 的 `wmb()` 语义），map() 之后 5 次、unmap() 之后
  9 次（每次都是所有表页，clean 幂等，比按 PTE 批量同步更保守）；coherent
  walker 一次都不调用；不挂 maintainer 时依旧一次都不调用（调用方负责）。
  maintainer 失败时 prepare() 拆掉全部表，map() 撤回刚写的页并把错误原样
  返回，unmap() 返回错误但叶子已清、重试会再发布一次。
- 随机扫：在 8 MiB 窗口里随机映射/取消映射约 400 段，然后把**每一页**都走一遍，
  断言 walk 出来的地址与当初 map 的地址一致。这一步的意义不是「跑通」，
  而是让 **map 与 walk 互相检查**：同一个位偏移错误很难在两边同时自洽。
- 整个文件都可以在 `--san` 下跑：越界、未对齐、释放后再用都会变成失败。

## 3. 这套测试能证明什么、不能证明什么

**能证明**：位布局与算术正确、边界与拒绝路径正确、调用方缓冲区不被污染、
所有权（每张表都还回去了）、内存安全（`--san`）、walk 与编码自洽，以及
**跨寄存器块**的先后（共享序号计数器，见 `test_bringup`）。

**不能证明**：MMIO 的读写顺序和时序、中断是否真的到来、时钟/电源是否真的开、
缓存是否在 GPU 看到之前写回、GPU 是否真的按这些描述符工作。
这些只能靠后面的阶段：先在 host 上用**假寄存器**验证时序，再上机只读，最后才写。

### `tests/test_job.cpp` —— v7 job 头与 WRITE_VALUE 链（纯内存）

- 没有任何寄存器：链建在一块 64 字节对齐的 host 内存里，逐字对照
  `third_party/mesa-panfrost/v7.xml` 的位段，不做任何 MMIO。
- Job Header（`v7.xml:1334-1351`）：全关的 32 位 Null 头标志字是 `1<<1`；
  全开、Index `0xabcd`、Type `10` 的头是 `0xabcddb15`（bit0 + `10<<1` +
  bit8/9/11/12/14/15 + `0xabcd<<16`），两个 Dependency 拼成 `0x56781234`，
  Fault Pointer 与 Next 各占两个字。`packHeader()` 会写满整个 64 字节槽
  （预填 0xaa 的内存里 payload 区全变 0），`unpackHeader()` 逐字段回读，
  空指针是 `BadArgument`。
- 第一个 WRITE_VALUE：标志字是黄金值 `0x00010005`（Is 64b、Type 2 << 1、
  Index 1 << 16），Address 字 8-9、Type 字 10、空字 11、Immediate 字 12-13、
  尾字 14-15 全 0；槽外一个字节都没动（预填 0xcc）。
- 立即数：Cycle Counter / System Timestamp / Zero 三种类型即使调用方传了
  立即数也写 0（`pan_jc_initialize_tiler()` 只写地址和类型）；Immediate 8/16/
  32/64 才存值，64 位值拆到两个字。类型 0 和 8 是 `BadArgument`，且不写
  任何字节、索引输出清零。
- 链：三个 job 的 Next 依次指下一个槽的 **GPU 地址**（不是 CPU 指针；另有
  一例 `0x7f00000000` 覆盖高 32 位），最后一个 Next = 0，索引 1/2/3。
- 容量：槽用完是 `NoResources`，上一个 job 的 Next 不变、索引清零；Index
  只有 16 位，所以第 65536 个 job 也是 `NoResources`，不截断成 0。
- 拒绝：未 prepare、空指针、CPU/GPU 未 64 字节对齐、字节数不是 64 的倍数、
  `gpu + bytes` 回绕；`reset()` 之后可以指向另一块内存，旧 job 保持原样。

### `tests/test_job_slot.cpp` —— JS 槽位提交、完成与软件 fence（假寄存器）

- MMU 是真的 `NativeMaliMmu`（`test_mmu` 已验过）挂在同一个假的
  `0x13040000` 窗口上，所以「这个 AS 已分配」是真实记账，JS 寄存器也落在
  GPU 会看到的位置。假寄存器块只加了三条真硬件行为：`JS_COMMAND_NEXT`
  写入后读回 0（命令被取走，否则源里的 `WARN_ON(job_read(JS_COMMAND_NEXT))`
  会让第二次提交永远 `Busy`）、`JOB_INT_CLEAR` 对 `JOB_INT_RAWSTAT` 是
  写 1 清零，以及 `JOB_INT_JS_STATE` 的第 j 位跟着 `JS_COMMAND_NEXT = START`
  置位、跟着 `JS_COMMAND = SOFT_STOP` 清零（`softStopStuck` 可以不让它清）。
  `doneAfterReads`/`doneWord` 让测试能说「第 N 次读状态时完成」。
- 写序：`JS_HEAD_NEXT_LO/HI` → `JS_AFFINITY_NEXT_LO/HI` → `JS_CONFIG_NEXT` →
  `JS_FLUSH_ID_NEXT` → `JS_COMMAND_NEXT = START`，共 7 次写；提交前读两次
  （`JS_COMMAND_NEXT` 的 staged 检查和 `JOB_INT_RAWSTAT` 的陈旧事件检查），
  后者只有在真有陈旧位时才多一次 `JOB_INT_CLEAR` 写。
- `JS_CONFIG` 黄金值：`as | 0x80000 | 0x300 | 0x3000 | 0x4000` = `0x87300`，
  第一次提交带链标志 `0x87b00`；`START_MMU`（1<<10）**不**在里面，因为
  `hw_issues_g52` 是 0。链标志按每个槽的 `emit_seqno` 奇偶：槽 0 的三次提交
  是 有/无/有，槽 1 的第一次又是有。
- 头部与 affinity 的 LO/HI：head `0x7_00000000` 拆成 `LO=0`、`HI=7`；
  shader 掩码 `0x3_0000000f` 拆成 `0xf`/`0x3`。
- 拒绝（全部零写）：未 prepare、没 setPresent、槽号 ≥3 或不在
  `GPU_JS_PRESENT`、AS 超出低 4 位或未分配、head 为 0、head 未 64 字节对齐、
  `JS_COMMAND_NEXT` 非 0（`Busy`）、电源门禁关闭（连读都不发生）。拒绝之后
  仍可提交，而且链序号没有变化。
- 回滚：flush id 写失败、第一个 head 写失败、START 自己写失败三种情况都断言
  「失败的那次写 + `JS_COMMAND_NEXT = NOP`」并以原错误返回；失败的提交不推进
- 一次一个 job：同一槽位在完成前再提交是 `Busy`，且不多写一个字节；完成之后
  才允许下一次，且链标志照旧翻转。
- 完成（DONE）：第 3 次状态读时锁存 `BIT(0)` → 3 次读、2 次 10 µs 睡、只写
  一次 `JOB_INT_CLEAR = BIT(0)`；`rawStatus = BIT(0)`、`jsStatus = 0`、
  outstanding 清掉；再等一次是 `NotReady` 且零读零写。
- 别的槽位：`BIT(1)` 挂着时等槽 0 会超时（4 次读、3 次睡），不写任何寄存器、
  不清 `BIT(1)`；随后锁存 `BIT(0)` 才成功，成功时也只清 `BIT(0)`。
- 超时：什么都不锁存 → `Timeout`，零写、outstanding 保持、未 faulted，可以
  重试；预算 0 时仍然读 1 次；`delay()` 返回错误时原样返回，outstanding 保持。
- 错误（ERR）：读 `JS_STATUS`（这里 `0xc2`，`NativeMaliFault::exceptionName()`
  解出 `TRANSLATION_FAULT_2`、`isFault()` 为真），写 `JS_COMMAND_NEXT = NOP`，
  再清 `BIT(16)`，返回 `IOError`；槽位锁为 faulted，`submit()` 是 `NotReady`
  且零读零写；`clearFault()` 只改状态（零 MMIO）、可重复调用、槽位不存在是
  `BadArgument`；解锁后下一次提交的链标志是偶数（失败的那个 job 已用掉序号）。
- DONE 与 ERR 同时置位：按错误处理，`JOB_INT_CLEAR` 写两个位；非 fault 的
  `TERMINATED`（0x04）也走这条路，`isFault()` 为假。
- 陈旧事件：提交前 `RAWSTAT` 已有本槽的 DONE/ERR → 提交多一次 `JOB_INT_CLEAR`
  （正好那两个位，且在 `JS_HEAD_NEXT_LO` 之前），随后等待会超时而不是立刻
  「完成」；别的槽位的陈旧位在提交时不会被清。
- 拒绝：空 `completion`、未 prepare、槽号越界、不在 `GPU_JS_PRESENT`、该槽没有
  在飞的 job、电源门禁关闭（零读），以及超时之后状态不变。
- 清理回滚：清 DONE 的写失败 → `IOError`，`rawStatus` 仍记录硬件说的话，
  outstanding 保持；重试成功。ERR 时 `JS_COMMAND_NEXT = NOP` 写失败 → 原因仍
  报告，但不清位、不锁 faulted，重试（幂等）后才锁。
- `clearFault()` 在有 job 在飞时是 `Busy`。
- **软件 fence**：`submit()` 的 out 参数拿到「槽位 + 序号」，序号就是源里
  `fence->seqno = ++queue[js].emit_seqno`（`panfrost_job.c:106`），所以每个槽的
  第一条是 1；同一个槽的第二次提交是 2，而槽 1 的第一条仍是 1。fence 在
  `waitForCompletion()` 收下那条 job（DONE **或** ERR）之前一直未 signaled，
  之后即使同一槽又提交了新 job 也保持 signaled；超时不算完成，重试把 job 收掉
  之后才 signaled。`clearFault()` 不回退序号（源里也不重置 `emit_seqno`）。
- **拒绝不留假 fence**：out 参数在入口就被清零，所以 `Busy` 的提交（同槽还有
  在飞的 job）不会让调用方把传进去的旧值当成新 job 的序号。
- **不会凭空 signaled**：无效 fence（`seqno == 0`）、槽号越界、以及比
  `latestSeqno()` 还大的序号都不算完成。最后一条不需要单独的「来自未来」判断：
  `completedSeqno()` 只可能是 `emitSeqno()` 的副本，永远不会跑在它前面。
- **复位**：`resetInterrupts()` 一次写 `JOB_INT_CLEAR = 0x70007`
  （`ALL_JS_INT_MASK`，三个槽的 DONE+ERR），假寄存器的 `RAWSTAT` 被清空；
  未 prepared 是 `NotReady`，写失败原样返回，电源门禁关闭时零写。
  `abandonOutstanding()` 在一个槽 faulted、两个槽在飞时清掉全部 outstanding
  与 fault 锁存，**不改** latest/completed 序号、不写寄存器、不读寄存器、
  可重复调用；被 reap 过的 ERR fence 仍是 signaled，被 reset 销毁的两条永远
  不是；之后三个槽都能再提交，序号从 2 继续而不是回到 1。
- **quiesce（soft-stop）**：未 prepared、只 prepare 没 setPresent 都是
  `NotReady` 零写；空闲时按槽序写 `JS_COMMAND_NEXT = NOP`、
  `JS_COMMAND = SOFT_STOP`（三个 present 槽共 6 写，`GPU_JS_PRESENT = 0x5`
  时只写槽 0/2 共 4 写），`JS_STATE` 已清所以不睡。提交中的链让 `JS_STATE`
  置位，SOFT_STOP 清掉后第一次轮询就成功；`softStopStuck` 时靠
  `RAWSTAT` 的 DONE 把槽从后续轮询里 mask 掉，仍然零睡。一直不清且没有事件
  → 1000 次 10 µs 睡后 `Timeout`，`JS_STATE` 保持不变。写失败、`JS_STATE`
  读失败都原样返回；电源门禁关闭时零写。

### `tests/test_memory.cpp` —— buffer span、映射与缓存策略（假分配器）

- 分配走 `NativeMaliMemorySource` 的假实现：一段页对齐的 host 内存，按
  `stride` 切成 span，span 之间故意留出 0x10000000 的物理空隙，所以叶子
  必须逐个 span 取地址，不能假设连续。
- `prepare()` 的拒绝：空源、0 字节、非 4 KiB 整数倍、非一致却无 maintainer、
  源自己的失败原样返回；这些情况都不留下分配。span 契约违反逐项覆盖：
  物理未对齐、有洞、重叠、覆盖不足、空 CPU 指针、0 个 span、超过 64 个
  span——每一项都断言“已经还回去了”。
- `map()`：16 KiB、两个 span 映射到 `0x20000`，断言 4 个叶子分别是
  `0x50000000/0x50001000/0x60000000/0x60001000` 的 `encodeLeaf(..., RW|Cached)`
  黄金值；随后是整段 flush 的 `AS_LOCKADDR = 0x2000e`（32 KiB 块）、`LOCK`、
  `FLUSH_PT`，`AS_STATUS` 读 3 次。重复映射返回 `Busy` 且零写。
- `unmap()`：4 个叶子清零、再 flush 一次；之后 `iova() == 0`、再次 `unmap()`
  是 `BadArgument`、分配仍在。
- 回滚：第二个 span 撞上已有叶子时第一个 span 被撤回，调用方原有的映射
  保留、`mappedPages()` 不变、零写；flush 超时时整个范围被撤回、返回
  `Timeout`、没有发出任何命令。
- `unmap()` 的 flush 超时后叶子已清但保持 mapped，重试只重复 flush。
- 拒绝：未 prepare、空 vm/mmu、未对齐 IOVA、未分配/不存在/越界 AS、
  prot 没有读也没有写、超出 48 位地址空间；全是零写零叶子。
- 缓存策略：非一致时 `cleanForDevice(0, 0x4000)` 是一次 `Clean`、带偏移的
  `invalidateForCpu` 把 CPU 指针推进同样的偏移；空范围成功且不调用原语；
  越界不裁剪；maintainer 的错误原样返回。一致 buffer（即使挂了 maintainer）
  两种操作都是 no-op；未 prepare 是 `NotReady`。

### `tests/test_mmu.cpp` —— AS 槽位与 MMU 使能/关闭（假 MMU 寄存器）

- 用**真的 `NativeMaliVm`**（已由 `test_vm` 对着 `io-pgtable-arm.c` 验过）产生
  `AS_TRANSTAB`/`AS_MEMATTR`，所以写进假寄存器的就是 GPU 真正会遍历的值：
  `TRANSTAB = 0x40000007`、`MEMATTR = 0x00888d88`、`TRANSCFG = 0`
  （`ADRMODE_LEGACY`）。
- AS 槽位：`setPresent(0x5)` 时只分配 0 和 2；满载返回 `NoResources`；
  归还后从最低位重新分配；未分配、越界、重复归还都拒绝。
- **复位后的槽位记忆**：`resetAllocation()` 把位图清零、`present(0x5)` 不变、
  旧的槽位不能再 `release()`、复位后仍从最低位分配；并且它是纯记账——零次
  读、零次写，即使写入开关关着、没 prepare 也能调用（`panfrost_mmu_reset()`，
  `panfrost_mmu.c:345-365`）。
- **复位后按号收回（`reclaim()`）**：复位清掉位图之后，原来的主人用同一个号
  把位放回去，其他号仍然空着（收回 0 和 2 之后 `allocate()` 是
  `NoResources`）；已经有人在用的号是 `Busy`（一个号不能有两个主人），不存在
  或越界的号是 `BadArgument`；同样零读零写。
- `lockRegionValue()`：与 `panfrost_mmu.c:103-137` 的 `fls64` / `ilog2` /
  `GENMASK_ULL` 算术逐例对照，包括整段 `~0` 编码为 63、不足 32 KiB 仍取
  32 KiB 块（14）、自然对齐块保留高位（`0x20000/0x20000 → 0x20010`）、
  size 0 和回绕拒绝。
- 使能写序：`AS_LOCKADDR` → `LOCK` → `FLUSH_MEM` → 等刷完 →
  `TRANSTAB`/`MEMATTR`/`TRANSCFG` 三对 LO/HI → `UPDATE`，共 11 次写；
  `AS_STATUS` 读 4 次（LOCK 前、FLUSH_MEM 前、刷完、UPDATE 前）。
  `activeReads` 模拟 `AS_STATUS_AS_ACTIVE`：先读两次 active 时总读数为 6、
  睡 2 次 10 µs；一直 active 且预算 0 时返回 `Timeout`，且只写了
  `AS_LOCKADDR`，没有发出任何命令。
- 拒绝：域/门未开（零写零读）、未 prepare、槽位未分配、槽位不存在、
  `NativeMaliVm` 未 prepare。
- 失败：`AS_COMMAND` 写失败时 `TRANSTAB` 从未被碰；`MEMATTR_HI` 失败后
  `enable()` 尽力 `disable()`，`TRANSTAB` 被清回 0，然后返回原错误；
  `AS_STATUS` 读失败和 `delay()` 失败原样返回。
- `disable()` 的写序和 11 次写、`TRANSCFG = ADRMODE_UNMAPPED`，并断言它
  **不**释放槽位。`FakeMmu` 只建模 `AS_STATUS` 的 active 读计数，其余是记录写
  的寄存器堆。
- `readFault()`：按 `panfrost_mmu.c:897-899` 读 `AS_FAULTSTATUS`、
  `AS_FAULTADDRESS_LO`、`AS_FAULTADDRESS_HI`（三次读、零写），对
  `source id 0xBEEF / decoder fault / READ / 0xC2 / 0x3_00001000` 的合成状态
  校验解码；不同 AS 的寄存器互不影响。不存在的槽位、越界槽位、空指针都是
  `BadArgument` 且零读；只 prepare 没 `setPresent`、M2 门关闭、未 prepare 都是
  `NotReady`；HI 读失败时 status 与 LO 仍保留。
- `flushRange()`：128 KiB 对齐范围的 `AS_LOCKADDR = 0x20010`，写序为
  `LOCKADDR` 对 → `LOCK` → `FLUSH_PT`，`AS_STATUS` 读 3 次；4 KiB 范围锁到
  32 KiB 最小块（14）。size 0、回绕、槽位未分配、槽位不存在、越界、
  `AS_ACTIVE` 不落（超时，只写了 `LOCKADDR` 对）、状态读失败都被覆盖。

### `tests/test_pmic.cpp` —— WACS2 与 MT6358（假 PWRAP）

`tests/FakeMaliPlatform.h` 里的 `FakePwrap` 是一个会讲 WACS2 协议的 16 位 PMIC
寄存器堆：命令字、FSM（idle / WFVLDCLR）、`INIT_DONE`、`VLDCLR` 都按驱动期望行为
建模，另外能注入「不 ready」「初始化未完成」「写被吞掉」三种故障。覆盖：

- 命令字构造：`(1<<31) | ((addr>>1)<<16) | data`，读命令 bit31 为 0；
  具体到 `0x1508`/`0x1526` 的黄金字值。
- 每次都先等 idle；读完成后写 VLDCLR，再等一次 idle；遗留的 WFVLDCLR 会在下一次
  命令前清掉，而不是被当成新答案。
- `INIT_DONE` 未置位 → `kIOReturnNotReady`；FSM 不进入 WFVLDCLR → 超时；
  读/写注入失败 → 错误原样返回。
- MT6358 选择器算术：`uV = 500000 + n*6250`，只接受精确步进；范围外、非整步进、
  `nullptr` 全部拒绝。
- VSRAM 先于 VGPU；已经是目标值时第二次调用一个 PMIC 写都不发。
- 写后回读不一致 → 恢复旧字并报 `kIOReturnIOError`（VGPU 失败时 VSRAM 也退回）。

### `tests/test_power.cpp` —— M2 序列（假 SPM/INFRA/TOPCKGEN/MFG_CFG/PWRAP）

`FakeSpm` 的 `PWR_STATUS` 由驱动写过的 `PWR_CON` 推导，`SRAM_PDN_ACK` 跟随
`SRAM_PDN`，并能设置「要过几次轮询才报告上电」和「永远不报告」；`FakeInfra`
在 `PROTECTEN_SET/CLR` 时记录另一个寄存器块的快照，用来钉死厂商的先后顺序。
`SRAM_PDN_ACK` 是**只读状态位**，所以 fake 在写入时把它从值里去掉：驱动用的是
读-改-写，若不屏蔽，fake 自己派生的 ack 会被写回 `PWR_CON` 并永久锁存，第二次
上电就会等一个永远不会清的 ack。`testPowerCycle()` 就是钉这一点的。
覆盖：

- `probe()`：八个原始字与两路 PMIC 电压/使能；未 prepare、空指针、pwrap 未初始化
  都被拒绝；probe 不改变任何配置字。
- 轮询：已经在位时不延时；超时按 `poll_us` 步进；`mask=0`、`poll_us=0`、空目标、
  延时失败都直接拒绝。
- 时钟源：先清 mux gate、再清 mfg_sel、写 0（clk26m）、脉冲 update；写失败即停。
- 时钟门：`BG3D` 清掉后回读为 0；已经为 0 时不重复写；状态位不消失或写失败都报错。
- 四个域：`async → mfg → core0 → core1`，每个域 `PWR_ON` 后轮询两位状态；
  已经上电的域不重复等。
- **厂商顺序**：`PROTECTEN_CLR` 发生时 MFG 的 `PWR_ON` 已置位、`SRAM_PDN` 仍为 0；
  超时回滚时 `PROTECTEN_SET` 发生在 MFG 断电之前。
- 超时回滚：MFG 永不报告上电 → 已起来的 async 和失败的 MFG 都关掉，core0/core1
  完全没被碰过。
- `bringUp()` 成功路径和「门控写失败 → 把四个域全部关掉、不报告 ready」。
- **下电**：`disableMfgClockGate()` 往 `SET` 写 `BG3D` 后状态位为 1；已经关着
  也照样写一次并成功（厂商的 `clk_disable_unprepare()` 是无条件的）；状态位不
  改变或写失败都是 `IOError`。`powerDownDomains()` 的写序是
  `core1 → core0 → mfg → async`（用跨寄存器共享序号计数器证明），每个域都留下
  `PWR_ON=0`、`ISO=1`、`RESET_B=0`，两个状态寄存器都读回“关”。
- **下电不跳步**：让 mfg 域的状态位永远不清 → `Timeout`，但 core1/core0/async
  仍然被关掉，返回的是第一个错误（形状同 `panfrost_gpu_power_off()`）。
- **`tearDown()`**：先关门再关域（门那次写的序号小于第一个域断电的序号），
  两个状态寄存器都关；PMIC 一个写都没有（两路 buck 故意不动，理由见
  `NativeMaliPower.h` 的 `tearDown()`）；再调用一次仍成功，可以重试。
- **门关不上也要关域**：`SET` 写失败时 `tearDown()` 返回 `IOError`，四个域照样
  断电。
- **整轮上电/下电可以重复**：`bringUp()` → `tearDown()` 连做两轮都成功，
  `gpuReadable()` 在每轮中间成功、结束后 `NotReady`；并断言 `PWR_CON` 里没有
  被读-改-写锁存的 `SRAM_PDN_ACK`。
- **GPU 读门**：四个域、topckgen mux gate、mfgcfg `BG3D` 三者都满足才返回成功；
  缺哪一项就置哪个位；读失败原样返回；未 prepare 返回 `NotReady`。
- `NativeMaliPower::kWriteAuthorized` 必须在测试构建里为 true；驱动构建为 false。

另外 `test_vm` 补了跨 2 MiB / 跨 1 GiB 边界的区间、48 位空间最后一页（可映射）与再下一页
（拒绝）；`test_board` 补了两个地址/大小 cell 超过 32 位的解析。

### `tests/test_bringup.cpp` —— M1→M4 的顺序与回滚（全部假块）

这是唯一一个**跨块**的测试：`FakeMaliPlatform.h` 的每个假寄存器块都挂同一个
`uint32_t sequence`，每次写通过 `record()` 取下一个号，所以断言可以写「A 块的
这次写早于 B 块的那次写」，而不只是每块内部的先后。每块自己的 `writes` 向量
仍然是该块内的顺序。

- **prepare**：不给 GPU 窗口是 `BadArgument`；没 prepare 就 `start()` 是
  `NotReady`；prepare 之后 `started()` 仍为假，MMU 也没有分配任何 AS。
- **M1 是前置条件**：`PWR_STATUS` 读失败时 `start()` 返回原错误，SPM/INFRA/
  TOPCKGEN/MFG_CFG/PWRAP/GPU 六个块**零写**。这是路线图「先读后写」的可执行版本。
- **跨块顺序**：PMIC 选择器 → topckgen mux 的父时钟 → 第一个域的 `PWR_ON` →
  mfgcfg 的门打开 → 第一个 GPU 写。前两个域和门都必须在 GPU 被写之前，
  因为 `probe()` 会先过 `gpuReadable()`；顺序错了 `start()` 自己就失败。
- **M3**：GPU 窗口的第一次写是 `INT_MASK = 0`（`panfrost_gpu_soft_reset()` 的开头），
  之后 `CMD = SOFT_RESET` < `L2_PWRON` < `SHADER_PWRON` < `TILER_PWRON`。
- **M4 不分配也不编程**：`start()` 成功后 `0x13040000` 窗口里没有任何落在
  `MMU_BASE..MMU_BASE+0x400` 的写，`allocatedMask()` 是 0 而 `present()` 是
  `0x7`。AS 由页表的主人自己取：`allocate()` 给最低空位（AS0），
  `enable(as, vm)` 才写寄存器，且 `TRANSTAB` 早于 `UPDATE`；再 `allocate()` 一次
  拿到 AS1 而不是同一个槽。
- **AS 还在手上时不下电**：分配了 AS 之后 `stop()` 是 `Busy` 且零写；只
  `disable()` 不 `release()` 仍然是 `Busy`；两者都做完之后 `stop()` 才成功。
- **MMU 拒绝**：没 prepare 的 VM 是 `NotReady` 且零写；没分配过的槽位是
  `BadArgument`；`start()` 之前（没有 present 掩码）`allocate()` 是 `NotReady`。
- **返回的观察值**：`gpuId == 0x70021000`、
  `model == "g52"`、`matched == 0x7002`、`asPresent == 0x7`、`ias/oas == 48/40`；
  `state.power` 是 **bring-up 之前**的读数（选择器 0 → 500000 µV），而
  `bringUp()` 之后 PMIC 已经是 18/56（612500/850000 µV）——这条断言正好把
  「probe 在写之前」钉死。`gpuReadable()` 在结束时不再缺任何一项。
- **stop 的反序**：主人先 disable AS（第二次 `UPDATE`、`TRANSTAB` 清 0）并
  release，然后 `stop()` 才是 `SHADER_PWROFF` → 门关 → 第一个域断电；顺序用共享
  序号断言。`mmu().allocatedMask() == 0` 而 `present() == 0x7`；再 `stop()` 一次
  成功且零写。
- **有 job 在飞**：`Jobs::Outstanding` 返回 `Busy`，GPU/SPM 一个写都没有，
  `started()` 仍为真（`panfrost_device_runtime_suspend()`，`panfrost_device.c:553-554`）。
- **每一个失败阶段都回滚**：未知 GPU id（`Unsupported`，但 ID 已记录）、
  软/硬复位都超时（`Timeout`）、L2 不 ready（shader/tiler 的 `PWROFF` 仍然发）、
  `GPU_AS_PRESENT == 0`（`BadArgument`）。每种情况都断言门已关、域已断电、
  `started()` 为假。
- **AS 关不掉时不是「槽位自由」**：disable 的 `UPDATE` 写失败，`stop()` 仍然是
  `Busy` 且 GPU/SPM 零写、块保持上电、分配位还在——GPU 可能还在走那张表，不能
  下电。硬件恢复后主人再 `disable()` + `release()`，`stop()` 才收尾；之后
  `start()` 从清空的分配位重新开始。
- **原始错误不被回滚覆盖**：GPU id 未知 + 门关不上 → `start()` 仍返回
  `Unsupported`，`teardownError` 是 `IOError`；此时对象认为块还没完全下来，
  所以 `start()` 是 `Busy`，修好门之后 `stop()` 才能收尾。

## 4. 给下一阶段加测试的模板

M2（电源/时钟）还没法上机，但「写寄存器的顺序」可以在 host 上先测死。
做法是把 MMIO 抽成一个接口（`NativeMaliPlatform`，见 `docs/OPEN_QUESTIONS.md` 第 5 条），
测试里给它一个数组：

```cpp
struct FakeRegisters final : NativeMaliRegisterTarget {
    struct Write { uint32_t offset; uint32_t value; };
    Write writes[64]{};
    size_t count = 0;
    uint32_t memory[0x4000 / 4]{};          /* 每个偏移一个假寄存器 */
    IOReturn write32(uint32_t offset, uint32_t value) override
    {
        if(count == 64)return kIOReturnNoResources;
        writes[count++] = {offset, value};
        memory[offset / 4] = value;
        return kIOReturnSuccess;
    }
    IOReturn read32(uint32_t offset, uint32_t *value) override
    {
        *value = memory[offset / 4];
        return kIOReturnSuccess;
    }
};
```

然后断言：先写哪个、后写哪个、轮询哪一位、超时几次后放弃、失败时回滚到哪个状态。
这三类断言（顺序、位、超时）就是 M2 上机前能把关的全部内容。
M3 的寄存器表已经按这个思路做了：`tools/generate-regs.sh` 生成，`tests/run.sh` 比对，
`tests/test_regs.cpp` 锁关键偏移。源文件一换，生成直接拒绝；生成物一改，`cmp` 先失败。

## 5. 报告失败怎么看

- `line 213: vm.tables() == 6` —— 第 213 行的断言不成立；左边是要证明的事实。
- `--san` 多出来的堆栈是越界/未对齐/释放后使用，属于真 bug，不要靠调大缓冲绕过。
- `released a page this source never handed out` —— 所有权错了：某张表被释放两次，
  或者释放了别人给的页。
- 解析类失败一律先看「失败时是否写了缓冲区」：那是最容易出现静默污染的地方。

## 6. 上机之后的第一条自检（M2 之后再谈）

M3 的第一条只读命令是 `GPU_ID`（`panfrost_regs.h` 里的 `GPU_ID`，偏移 0x00），
这台机器上必须是 **`0x70021000`**（product `0x7002` = G52，revision `r1p0`；
见 `docs/DEVICE.md` 第 2 节，`tests/test_gpu.cpp` 用它做黄金值）。**但在这台机器的
MFG 域确认上电之前不要读 `0x13040000`**：未上电读 MMIO 可能总线挂死，这是路线图
风险表第 1 条。M2 的 `probe()` 先读 SPM 状态，正是为了在碰 GPU 之前知道域是否已开。

## 7. `xnu/` 不在这套测试里

`tests/run.sh` 只编译根目录的 `NativeMali*.cpp` 和 `tests/test_*.cpp`，这是
故意的：`xnu/` 依赖 XNU 头文件和真实 IOKit 对象，host 上编不了。它没有覆盖
任何一条这里列出的断言，**这里全绿也不说明胶水层能工作**。

- 胶水里每个未验证的假设就地标 `UNVERIFIED(XNU)` / `UNVERIFIED(HW)`，
  清单和删除条件在 `docs/UNVERIFIED.md`。
- 唯一做过的机器检查是 `-fsyntax-only` 加本地临时 IOKit 桩（不提交），它只
  证明本仓库自己的 C++ 自洽。桩的签名不是真实 XNU 的签名。
- 真正的验证只有一条路：在 UniXNU-merlinx 的树里编译、安装、看 `start()` 的
  probe 日志、用 `ioreg -c NativeMaliGpuService` 确认实例，然后逐条删标记。
