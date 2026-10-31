<!-- SPDX-License-Identifier: GPL-2.0-only -->
# `UNVERIFIED(XNU)`：纸面代码的标记

这个仓库没有 kext 构建，`tests/run.sh` 也不编译 `xnu/`。为了在真机/XNU
接好之前也能把 IOKit 那一层写出来，凡是**没有在本仓库里编译过、也没有在
真机上跑过**的地方都用统一标记：

```text
UNVERIFIED(XNU)   依赖 XNU 头文件、运行时行为或内核服务
UNVERIFIED(HW)    依赖这台手机的实际硬件行为
```

标记是注释，不是宏：它不会改变代码，只让 `grep -rn 'UNVERIFIED('` 一次列出
所有没被证明的地方。规则只有一条：

> 标记只有在**目标上真的编译并运行过**之后才能删除，并且在提交信息里写出
> 用的命令、看到的结果。单元测试通过、`ioreg` 能看到类、或者「和参考驱动
> 写法一样」都不算。

`xnu/` 里的代码是**胶水**：所有决定（寄存器、页表、job、fence、ABI 记录）
都在纯层，纯层有 host 测试。这里只做四件事：映射窗口、分配内存、复制记录、
加一把锁。即便如此，下面每一条在被证明之前都可能是错的。

## 清单

### 整个 `xnu/` 目录

`xnu/NativeMaliXnuSupport.*`、`xnu/NativeMaliGpuService.cpp`、
`xnu/Info.plist` 从未在 XNU 头文件下编译过，也从未安装。没有任何 grep 结果能
说明它们能过真实内核的头文件。

- 唯一做过的机器检查是：在 `build/xnushim/`（本地、未提交、`build/` 已忽略）
  里放一组最小的 IOKit/内核桩，然后
  `c++ -std=c++20 -fsyntax-only -Ibuild/xnushim -I. xnu/*.cpp`。它只证明
  本仓库自己的 C++（成员名、访问控制、类型）自洽，**不**证明桩的签名和真实
  XNU 一致，更不证明能链接或运行。桩文件不在仓库里，也不该被提交。
- 要证明：在 UniXNU-merlinx 的树里编译成 kext，`ioreg -c
  NativeMaliGpuService` 能看到实例，`start()` 的日志能打出来。
- 参考的是 `NativeAdreno`（MIT）在同一棵树里的用法：
  `IODeviceMemory::withRange`、`IOBufferMemoryDescriptor::inTaskWithPhysicalMask`、
  `getPhysicalSegment(..., kIOMemoryMapperNone)`、`IOExternalMethodDispatch`、
  `clientMemoryForType` 的 view 缓存。参考能编译不代表这里能。

### 固定窗口地址

`NativeMaliGpuService.cpp` 的 `kSpmBase` / `kInfracfgBase` / `kTopckgenBase` /
`kMfgcfgBase` / `kPwrapBase` 来自 vendor DTS 和 `NativeMaliPower.h`（有出处），
但**怎么拿到这些窗口**没有证明：

- 现在用 `IODeviceMemory::withRange()` 按物理地址映射，`UNVERIFIED(XNU)`。
  真机上这些块可能已经被别的驱动占用，或者 XNU 的 ADT 不允许这样映射。
- 另一种做法是走 DT phandle（`clocks` 指向 topckgen/mfgcfg/scpsys），但那需要
  XNU 的 DT 属性 API，见 `docs/OPEN_QUESTIONS.md` 第 5 条。
- GPU 窗口本身来自 provider 的 `reg`（`getDeviceMemoryWithIndex(0)`），并校验
  基址等于 `0x13040000`；这个校验在 ADT 编码不同（例如补了别的窗口）时会拒绝
  发布，而不是映射错块。

### `#address-cells` / `#size-cells`

`cellProperty()` 缺省 2/2。这个 DT 的 mali 节点确实是 2/2（`NativeMaliBoard.h`），
但如果 ADT 生成器把属性搬成了别的形式，缺省会静默地按 2/2 解析。要证明：
在真机 `ioreg` 里看到 provider 的 `reg` 长度是 16 字节。

### `IODelay`

`NativeMaliXnuWindow::delay()` 用 `IODelay(microseconds)`。10 µs 的轮询需要的是
自旋而不是 `IOSleep`，但是否允许在持锁的上下文里 `IODelay`、以及它是否真的
至少等这么久，没有在本内核上验证。

### 页表与 buffer 的分配

`NativeMaliXnuPages` / `NativeMaliXnuBuffers`：

- `kIOMemoryPhysicallyContiguous` 是刻意的：纯层只接受 64 个 span，4 KiB 页的
  散列 buffer 在 256 KiB 就会超限。代价是大块分配更容易失败。这个取舍没有在
  真机上试过；失败会明确返回 `NoResources`，不会截断。
- 物理掩码用 `(1 << oas) - 1`，oas 来自 M3 probe。probe 没回答时按 40 位
  （Mali 上限）算。
- `prepare()` / `complete()` 的配对来自 `NativeAdrenoMemory.cpp`。`complete()`
  失败时故意泄漏描述符（页还钉着，放回去比泄漏更危险），日志里会写。
- buffer 在交给 GPU 前被清零；这是本驱动的策略（未初始化页被 GPU 读到时应是
  确定的 0），不是源的要求。

### 缓存维护

`NativeMaliXnuCache` 用 `dc cvac` / `dc civac` / `dc ivac` + `dsb sy`，整页
invalidate，clean 向外取整到页。序列和整页规则来自 `NativeAdrenoCpu.cpp`
（同一棵树的驱动），但：

- 没有在这台手机的 XNU 上跑过一次，也没有和 `pmap` 的 cache attribute 交互过。
- 用户态 BO 的映射是**缓存**的（`kIOMapAnywhere`，没有 inhibit），所以策略
  放在胶水里：`BoCpuUnmap` 成功后 clean（CPU 写的让 GPU 看见），`BoCpuMap`
  成功后 invalidate（GPU 写的让 CPU 看见）。这个策略没有经过真机的一致性
  测试；`docs/OPEN_QUESTIONS.md` 第 5 条仍是真正的未知项。
- `clientMemoryForType()` 对同一个 token 返回同一个描述符（参考驱动的
  view 缓存）。`IOConnectUnmapMemory64` 会再问一次，返回新描述符会让 unmap
  变成 no-op，用户映射活过 `BoFree`，然后释放带映射的页。这个结论来自参考
  驱动的 panic 记录，本仓库没有复现过。
- 因为这个风险，`BoCpuUnmap` 成功后**不**丢掉 view，只有 `BoFree` 成功和
  连接关闭时才丢：`IOConnectUnmapMemory64` 还要用同一个描述符。参考驱动在
  丢 view 之前用 `removeMappingForDescriptor()` 强制拆掉用户映射，本仓库没有
  这个辅助函数，所以这里**没有**等价保护。约定是客户端先
  `IOConnectUnmapMemory64` 再 `BoFree`；胶水层无法检查，也无法强制。
  `UNVERIFIED(HW)`：这个约定和「不丢 view」的组合没有在真机上跑过。

### 锁与生命周期

- 一把 `IOLock`（service 的 `sessionLock()`）串行化所有 `dispatch`、
  `clientMemoryForType`、open/close 和 stop。`NativeMaliUserClient` 和
  `NativeMaliJobSlot` 都不是线程安全的，这把锁是唯一保证。
- `stop()` 持锁调用每个客户端的 `shutdownLocked()`；`close()` 失败时保留
  `Session`、注册关系和地址空间，不把 GPU 下电。`free()` 在这种情况下泄漏
  `Session`，因为纯 service 还存着指向它的指针。
- `free()` 也会去拿同一把锁。如果 IOKit 在 `stop()` 持锁时同步调用
  `free()`，会死锁。没有在真机上验证调用顺序。
- IOKit 对象是否执行 C++ 析构（以及成员析构）没有在本内核上验证，所以
  `IOMemoryMap` 和 `IOLock` 都在 `free()`/`stop()` 里显式 release，而不是靠
  成员析构；`device_.stop()` 失败时 `releaseWindows()` 会保留映射，避免把
  还没拆完的 GPU 的窗口抽走。
- `newUserClient()` 自己 `attach(this)` + `start(this)` 客户端，再 `mount()`；
  这是 `NativeAdrenoPublish.cpp` 的做法，注释说 `IOService::newUserClient`
  会做这两步。覆盖了 `newUserClient` 之后由谁负责、以及 `start` 的返回值在
  真实打开路径里是否被检查，都没有在本内核上验证。失败路径用
  `terminate()`/`detach()` 回收，语义同样未验证。

### `Info.plist`

- `CFBundleIdentifier = org.dracen.NativeMali`、`CFBundleExecutable =
  NativeMali`、`IOClass = NativeMaliGpuService` 都是纸面值；bundled 名称
  取决于 UniXNU 的打包规则。
- 没有 `IOPropertyMatch`：provider 的 `compatible` 是字符串列表（OSData），
  用 OSString 匹配不会命中；类在 `probe()` 里自己检查 `mediatek,mali`。
  这一点是从 DT 的表示推出来的，没有在真机 `ioreg` 里验证。

### ABI 方法表

`externalMethod()` 每次调用按 `inputSize()`/`outputSize()` 填一条栈上的
`IOExternalMethodDispatch`，所以表和纯层不可能漂移。IOKit 是否接受栈上的
dispatch、以及 `checkStructureInputSize/OutputSize` 的类型宽度，没有在本内核
上编译验证。

`NativeMaliMethodReset`（方法 11）是设备级方法，没有输入/输出记录，所以在
`IOExternalMethodDispatch` 里两个尺寸都是 0。任何已连接的客户端都能调用它；
它会让**所有**客户端的在飞 fence 变成 `IOError`。这个并发语义只有 host 测试
（单个进程、假寄存器），没有在真机上用两个客户端验证过。

### 未接上的部分

- 复位现在有 ABI 入口：`NativeMaliMethodReset`（方法 11，无记录）由
  `NativeMaliUserClient::dispatch()` 转给 `openClient()` 注入的
  `NativeMaliResetTarget`，也就是 `NativeMaliService::reset()`；胶水的
  `action()` 不需要按选择器增加分支，`allocateDispatch()` 的表会接受它。这条
  路径仍按纸面代码对待，直到在真机上执行过一次 Reset 并确认设备随后可用。
- 中断：`NativeMaliIrq` 只有解码，`IOInterruptEventSource` 没有写，完成仍是
  轮询（`docs/OPEN_QUESTIONS.md` 第 14 条）。
- `stop()` 之后的重新 `start()` 没有在真机上试过；纯层有测试，但 IOKit 的
  attach/terminate 语义没测。
