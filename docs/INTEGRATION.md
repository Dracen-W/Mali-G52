<!-- SPDX-License-Identifier: GPL-2.0-only -->
# 对接进 UniXNU-merlinx：改哪几处、按什么顺序

这份文档只讲**另一侧**要做的事，也就是让 `NativeMali` 在 XNU 里被
`IOPlatformDevice` 匹配到、被编译进去。本仓库的代码一行都不用为它改。

原则：**这个仓库不碰 `UniXNU-merlinx`**。下面每一步都写成可以直接复制的
命令或补丁；改哪个文件、加哪几行、为什么加，都写清楚。

> 命名说明：ADT 生成器目前叫 `scripts/make-merlinx-adt.py`（上游文档里也有
> 直接叫「ADT tool」的）。如果你说的是 `scripts/build-arm-driver.py`，第 3 节
> 就是它；两个都要改，顺序是先 3 后 4。

> 2026-10-31 更新：`UniXNU-merlinx` 清理后，归档入口改成组件目录 +
> `profiles/*/profile.toml` 的 `component:<id>`，`build-arm-driver.py` 也从
> catalog 读取而不是写死名单。以 `docs/KEXT_BUILD.md` 第七节的清单为准；本文的
> `build-arm-driver.py` 代码片段保留作历史说明。

## 0. 现状与前提

| 一侧 | 现成的东西 | 还缺的东西 |
| --- | --- | --- |
| 本仓库 `Dracen-W/Mali-G52` | M0–M6 全部 host 测过的纯逻辑层、`NativeMaliRegs.h`、`NativeMaliBringup`、`xnu/` 纸面胶水与 `Info.plist` | 按内核真实 ABI 编译过的胶水；构建前置条件见 `docs/KEXT_BUILD.md` |
| `UniXNU-merlinx` | ADT 生成器、`board.toml`、`build-arm-driver.py`、`ArmDTPlatform` | 三处接入点（下面 1、3、4） |

所以**接入的前置条件在本仓库**：胶水必须能按内核真实参数编译（M7）。
`xnu/NativeMaliGpuService.cpp` 已存在，但只在本地桩头文件下做过语法检查；
在它进入 `build-arm-driver.py` 之前，只做第 1、2、4 步是安全的（一个没有驱动
认领的节点不会匹配到任何类）。`build-arm-driver.py` 是 `-Werror` 编译，
源文件不存在或有一处 ABI 不匹配就直接失败。

## 1. 把本仓库变成子模块

上游的驱动都是 `drivers/<名字>` 下的子模块（`.gitmodules` 里
`drivers/ArmCpu`、`drivers/NativeAdreno` …… 都是这个形状）。

```sh
cd ~/Desktop/merlinx-work/UniXNU-merlinx
git submodule add https://github.com/Dracen-W/Mali-G52.git drivers/NativeMali
git commit -m "feat(native-mali): add the NativeMali driver submodule"
```

`drivers/NativeMali` 里的目录结构就是本仓库根目录，不要复制文件：子模块钉的是
commit，`git submodule update --remote` 才前进。M2/M3/M4/M6 的写入开关默认仍是
`0`，所以拉进来不会写任何寄存器。

## 2. ADT 里给 Mali 一个节点（`scripts/make-merlinx-adt.py`）

XNU 在 merlinx 上用的**不是** MTK 那个 DTB，而是这个脚本生成的 Apple 格式树
（`Payloads/kernel.macho.adt`）。`ArmDTPlatform` 会给 `/arm-io` 的**每个子节点**
发布一个 `IOPlatformDevice`，驱动的 personality 靠 `compatible` 匹配上去
（`docs/drivers/device-tree-matching.md` 的「Platform device by compatible」）。

### 2.1 最小改法（推荐先做这个）

只加 `name` 和 `compatible`，**不加 `reg`、不加 `interrupts`**：

```python
        node('arm-io', [('device_type', b'arm-io\0'),
                        ('#address-cells', struct.pack('<I', 2)),
                        ('#size-cells', struct.pack('<I', 2)),
                        ('ranges', struct.pack('<QQQ', 0, ARMIO_BASE, 0x00400000))],
             [node('intc', [('compatible', b'arm,gic-v3\0'),
                            ('reg', struct.pack('<4Q', GIC_DIST - ARMIO_BASE, GIC_DIST_SIZE,
                                                GIC_REDIST - ARMIO_BASE, GIC_REDIST_SIZE)),
                            ('redistributor-stride', struct.pack('<Q', 0x40000))]),
              # MT6768 的 GPU。名字与 compatible 取自 tests/data/mt6768.dtb 的
              # /mali@13040000，这里只做匹配；地址由驱动按板级常量取，理由见 2.2。
              node('mali', [('compatible', b'mediatek,mali\0arm,mali-valhall\0')])]),
```

- `compatible` 的两个字符串顺序照抄被钉死的 DTB：`"mediatek,mali"` 在前，
  `"arm,mali-valhall"` 在后。
- `IONameMatch` 用 `mediatek,mali` 就行（它比对 provider 名字**和** compatible
  列表）；想更严格可以用 `arm,mali-valhall`，两者都只出现在这一台机器的 GPU 上。
- 节点名写成 `mali` 而不是 `mali@13040000`：`@` 后面的单元地址只有在带 `reg`
  时才有意义。

### 2.2 为什么先不加 `reg`：`arm-io` 的窗口够不到 0x13040000

现在 `ranges` 只有一段 `(child 0, parent 0x0C000000, size 0x400000)`，即
`0x0C000000–0x0FFFFFFF`。GPU 在 `0x13040000`，**在窗口外**。UniXNU 的
`docs/porting/device-tree.md` 对这种情况写得很明确：

> A device below the window base cannot be expressed as a child; the driver then
> takes its physical addresses from board constants.

所以 M7 的 `NativeMaliGpuService` 直接按板级常量映射（`0x13040000` GPU 窗口、
`0x10006000` SPM、`0x10001000` INFRACFG_AO、`0x10000000` TOPCKGEN、
`0x13000000` MFG_CFG、`0x1000D000` PWRAP），全部来自
`tests/data/mt6768.dtb` + `docs/DEVICE.md`，有哈希可查。

如果以后坚持让 DT 当唯一权威，再加第二段 `ranges` 和 `reg`：

```python
('ranges', struct.pack('<QQQ', 0, ARMIO_BASE, 0x00400000) +
           struct.pack('<QQQ', 0x13040000, 0x13040000, 0x00040000)),
...
node('mali', [('compatible', b'mediatek,mali\0arm,mali-valhall\0'),
              ('reg', ...)]),
```

**注意**：ADT 的 `reg` 是 Apple 自己的编码，不是标准 FDT 的
`#address-cells × 32 位`。上面 `intc` 那行用 `struct.pack('<4Q', ...)` 能工作，
但这只证明「GIC 那一段这么写是对的」；给 mali 加 `reg` 时必须照 `intc` 的模式
写，并且**在真机上用 `ioreg` 确认 driver 读到的地址正确**之后再依赖它。
在那之前，板级常量是唯一被证明过的路径。

### 2.3 中断：等 `NativeMaliIrq` 落地再加

现在完成判定是轮询 `JOB_INT_RAWSTAT`（`docs/TODO.md` M6），驱动不认领中断，
所以不加 `interrupts` 也能跑通第一个 WRITE_VALUE。等要做中断路径时，值照抄
DTB：

```
interrupts     = <0 274 8  0 275 8  0 276 8  0 277 8  0 278 8>   (2 cells each)
interrupt-names = "GPU", "MMU", "JOB", "EVENT", "PWR"
flags 0x8 = IRQ_TYPE_LEVEL_LOW
```

编码同样要照 `intc` 的写法验证一次（见 2.2 的注意）。DTB 原文与「为什么是电平低」
记在 `docs/OPEN_QUESTIONS.md` 第 2 条。

## 3. `board.toml`：让它进内核构建

`[kernel] archives` 列表就是构建时依次跑的命令（`board.toml:31-44`）。加一行：

```toml
[kernel]
archives = [
  "build-corecrypto-14.py",
  "build-corecrypto-14.py --namespace",
  "build-arm-driver.py ArmDTPlatform",
  "build-arm-driver.py ArmCpu",
  "build-arm-driver.py NativeAES",
  "build-arm-driver.py NativeMali",      # <- 新增
  ...
]
```

`scripts/build-arm-driver.py` 是**单个 TU 的 ABI 编译检查**（用
`build/xnu/ios-obj/.../IOPlatformExpert.cpo.json` 里记录的原始编译参数，
再加 `-Wall -Wextra -Werror`）。它把 `component` 当枚举用，所以要改四处 ——
前三处是这段脚本里的三行，第四处是头文件搜索路径（见下）：

```python
# 1) choices 里加一项（第 11 行那一长串的末尾）
parser.add_argument('component', choices=[..., 'NativeKeyStoreEvents', 'NativeAdreno', 'NativeMali'])

# 2) 源文件映射（第 28 行）：默认是 drivers/<component>/<component>.cpp，
#    我们只有一个入口 TU，所以往那个 dict 里加一项（NativeAdreno 也是这么做的）
... {'NativeAdreno':'NativeAdrenoService.cpp',
     'NativeMali':'xnu/NativeMaliGpuService.cpp'}.get(component, component + '.cpp')

# 3) 输出目录映射（第 32 行）：往 dict 里加一项
... 'NativeAdreno':'native-adreno', 'NativeMali':'native-mali'}[component]

# 4) NativeAdreno 那一段（第 84 行）旁边加一个分支，让 #include "NativeMali*.h"
#    能解析。子模块就是 drivers/NativeMali；入口 TU 在它的 xnu/ 下，
#    根目录还有 NativeMali*.h：
if component == 'NativeMali':
    command += ['-I', str(ROOT / 'drivers/NativeMali'),
                '-I', str(ROOT / 'drivers/NativeMali/xnu')]
```

第 4 处要不要加，取决于 `IOPlatformExpert.cpo.json` 里记录的命令本来有没有把
`drivers/<component>` 放进 `-I`。NativeAdreno 显式加了自己的几个 `kits/` 路径，
说明那条记录里没有；先按上面加上，构建成功就不必再动。

这就是为什么第 0 节说胶水必须先能编译：`-Werror` 下源文件不存在或有一个警告，
整个 `build kernel` 就失败。

`xnu/NativeMaliGpuService.cpp` 只 include 头文件；其余 `NativeMali*.cpp` 由谁编译、
如何链进内核，照 `NativeAdreno` 现在的方式办（`NativeAdreno` 的 29 个 `.cpp` 也
不是这个脚本编的）。本仓库的 `NativeMali*.cpp` 都不 include 别的 `.cpp`，所以
它们各自独立编译即可，`tests/run.sh` 就是这么做的。多 TU 归档成
`libNativeMali.a` 的做法见 `docs/KEXT_BUILD.md` 第四节。

## 4. `Info.plist`（只有 kext 路线需要；内置驱动不用）

`IOClass` 必须是真的类名，不然第一次上机只会多一条无用的失败日志
（`docs/TODO.md` M1 的最后一条）。代码里的类是 `NativeMaliGpuService`
（`xnu/NativeMaliGpuService.cpp`），下面的示例已与它一致；`IOProviderClass`
和匹配规则仍未在目标上验证（见 `docs/KEXT_BUILD.md`、`docs/UNVERIFIED.md`）。

```xml
<key>IOKitPersonalities</key><dict><key>merlinx MT6768 Mali</key><dict>
  <key>IOClass</key><string>NativeMaliGpuService</string>
  <key>IOProviderClass</key><string>IOPlatformDevice</string>
  <key>IONameMatch</key><string>mediatek,mali</string>
  <key>IOProbeScore</key><integer>1000</integer>
</dict></dict>
```

`IOProbeScore` 与 NativeAdreno 一致（1000）；`CFBundleIdentifier` 用
`org.dracen-w.NativeMali` 之类，`OSBundleRequired` = `Root`。

## 5. 顺序

1. 本仓库：让 `xnu/` 胶水按内核真实参数编译通过（M7），`tests/run.sh` 仍全绿；
   需要什么见 `docs/KEXT_BUILD.md`。
2. `UniXNU-merlinx`：第 2.1 节的 ADT 节点。
3. `UniXNU-merlinx`：第 3 节的 `build-arm-driver.py` 三处 + `board.toml` 一行。
4. `UniXNU-merlinx`：第 1 节的子模块。
5. 构建、刷机，用第 6 节的自检确认驱动被匹配到，**这时还没有任何写操作**。

第 2 步和第 4 步可以提前单独做；它们只多一个没人认领的节点/目录。

## 6. 接进去之后怎么确认（不写寄存器）

```sh
# 节点在不在、属性对不对
ioreg -l -p IODeviceTree | grep -i mali
# 驱动有没有绑上（IOClass 出现说明 probe 跑过）
ioreg -r -c NativeMaliGpuService -l
```

`NativeMaliGpuService::start()` 的第一段（M1）只读：`NativeMaliPower::probe()` 读
SPM 状态、INFRACFG protect、topckgen 的 `CLK_CFG_1`、mfgcfg 门状态和两路 PMIC
电压。**先看这一段的输出**，把 `docs/DEVICE.md` 里「等上机」的几条补上，再谈
`NATIVE_MALI_M2_WRITES`。

`NativeMaliBringup::start()` 已经把这个顺序写死成一个调用：
读平台 → 电压 → 时钟源 → 四个域 → 时钟门 → `GPU_ID` → 软复位 → L2/shader/tiler
上电 → 记 `GPU_AS_PRESENT` 并清掉旧的分配位。任何一步失败都会逆序回滚并返回
**原始错误**，回滚自己的错误从 `teardownError` 出来，两个都不会被吞掉。
地址空间不在这里分配：纯的 `NativeMaliService` 在 `openClient()` 里给每个客户端
`allocate()` + `enable()`（页表也属于它），`closeClient()` 里 `disable()` +
`release()`；块不可读时（复位后电源没回来）`closeClient()` 走无寄存器的拆除，
直接还 BO、release 号、拆表。只要还有客户端（也就是还有 AS 在手上），`stop()`
是 `Busy` 且零写。GPU 复位也由它编排，所以 IOKit 子类只需要：把 DT/MMIO 取到
的块 `prepare()` 进去、在 `open` 时调 `openClient()`、在 `terminate` 时调
`closeClient()`、每个 ABI 方法 dispatch 前先问 `clientUsable()`。

## 6.5 仓库里的 `xnu/` 纸面胶水

`xnu/NativeMaliXnuSupport.{h,cpp}`、`xnu/NativeMaliGpuService.cpp` 和
`xnu/Info.plist` 是 IOKit 那一层的实现，但**本仓库的构建不编译它们**，也没有
在真机上跑过。`tests/run.sh` 只编译根目录的 `NativeMali*.cpp` 和
`tests/test_*.cpp`，所以这些文件不会参与 host 测试；它们的价值是让「接进去」
从设计变成一份可以直接放进 UniXNU 树里试的代码。

- 每个没被证明的调用就地标了 `UNVERIFIED(XNU)` 或 `UNVERIFIED(HW)`，说明文字
  和「删除标记的条件」在 `docs/UNVERIFIED.md`。做第 6 节的确认时，先
  `grep -rn 'UNVERIFIED(' xnu/`，从第一处开始。
- 文件里没有 `IOInterruptEventSource`，也没有复位入口；完成仍是轮询
  （`docs/OPEN_QUESTIONS.md` 第 14 条）。`Info.plist` 的 `IOClass` 是
  `NativeMaliGpuService`，`CFBundleExecutable` 是 `NativeMali`，这两个名字要
  跟着 UniXNU 的打包规则改。
- `start()` 在 M2/M3 写入未打开时会在只读 probe 之后返回
  `kIOReturnUnsupported` 并拒绝发布，但 probe 的原始值已经打进日志；这和
  第 6 节的「先看这一段的输出」是同一件事。

## 7. 这份文档不决定的事

- **日志往哪打**：这台机器没有 UART；XNU 进到 launchd 之后用 `IOLog`/`dmesg`
  还是别的方式，取决于你那边现在的调试通道。
- **`IOPlatformDevice` 的 `reg` 编码**：见 2.2，需要用真机 `ioreg` 验证一次，
  不靠猜。
- **中断路径**：`NativeMaliIrq` 目前只有状态字解码（`NativeMaliIrq.h`），没有
  处理器，也没有认领中断，所以 2.3 的属性先不加。
- **`NativeMaliMemory` 的 XNU 后端**：`IOBufferMemoryDescriptor` 的分配与缓存
  原语仍是 `docs/OPEN_QUESTIONS.md` 第 5 条，等 XNU 侧确认后再写成真实实现。
