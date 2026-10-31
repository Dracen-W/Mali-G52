<!-- SPDX-License-Identifier: GPL-2.0-only -->
# 内核侧构建需要什么（内置驱动 vs kext）

先纠正一个名词，因为它决定要准备什么：

- **内置 XNU 驱动**（本仓库 `docs/INTEGRATION.md` 走的路线）：驱动是 C++ IOKit
  类，编成一个静态归档链进内核，个性写进 `iokit/KernelConfigTables.cpp`。
  没有 kext bundle，也没有 `Info.plist` 参与启动。
- **kext bundle**：标准 `.kext` 目录，由 UEFI 暂存、由实现了
  `UNIXNU_LOADER_PROTOCOL` 第 2 版的 loader 载入，AMFI 校验签名与 trust cache。

本仓库现在有 `xnu/NativeMaliGpuService.cpp`（788 行纸面胶水）、
`xnu/NativeMaliXnuSupport.{h,cpp}` 和 `xnu/Info.plist`。前两个可以用本地
`build/xnushim/`（非 IOKit 真头文件，只是桩）做语法检查，已通过；**三者都从未
用真正的内核 ABI 编译过，plist 也从未安装过**（`docs/UNVERIFIED.md`）。

## 一、内置驱动：需要什么

这条路不需要 kext loader，也不需要签名；它需要的是**能进入 UniXNU 的内核构建**。

| 需要 | 现状 | 说明 |
| --- | --- | --- |
| UniXNU 内核构建树 | 本机有 `UniXNU-merlinx`，但没确认它是否已构建到 `build/xnu/` | `scripts/build-arm-driver.py` 依赖 `build/xnu/ios-obj/.../*.cpo.json` 里记录的原始编译命令；没有这棵树，脚本跑不了 |
| 内核源码 revision | 未记录 | UniXNU 文档写的是 `xnu-10063.141.1` / iOS 17；需要图像实际使用的那一个 |
| `board.toml` 一行 | 未改（按约定不动该仓库） | `[kernel] archives` 加 `build-arm-driver.py NativeMali` |
| `build-arm-driver.py` 四处 | 未改 | 组件枚举、源文件映射、输出目录、`-I drivers/NativeMali` |
| 子模块 | 未加 | `drivers/NativeMali` 指向 `Dracen-W/Mali-G52` |
| 内置个性 | 未写 | `iokit/KernelConfigTables.cpp` 的 `IOClass = NativeMaliGpuService`、`IOProviderClass`、`IONameMatch = mediatek,mali`；`xnu/Info.plist` 只是参考，不参与启动 |
| ADT 节点 | 未改 | `scripts/make-merlinx-adt.py` 加 `node('mali', compatible=...)`；地址仍走板级常量（窗口够不到 `0x13040000`） |
| 刷机与日志回路 | 只有你能做 | `IOLog`/串口/pstore 至少要有一条能看到；否则第一次 `start()` 失败也只能靠猜 |
| XNU 能力确认 | 未确认 | `docs/OPEN_QUESTIONS.md` 第 5 条：DT 属性读取、物理 MMIO 映射、`IOInterruptEventSource`/GIC、`IOBufferMemoryDescriptor` 的物理 span、缓存维护原语 |

最小可编译闭环只需要前两行：**内核构建树（含一组 `.cpo.json`）**。有了它，
`build-arm-driver.py NativeMali` 就能按内核真实参数编译本节胶水，`-Werror` 会
把任何 ABI 不匹配暴露出来。剩下的 ADT/`board.toml`/子模块是“让它在目标上被
匹配到”的步骤，可以之后再加。

## 二、kext：在此之上还需要什么

| 需要 | 现状 | 依据 |
| --- | --- | --- |
| 实现 `UNIXNU_LOADER_PROTOCOL` 第 2 版的 loader | 没有 | `UniXNU-merlinx/docs/drivers/kexts.md`：loader 收到 `UNIXNU_LOADER_KEXT`，且“shipped DarwinBoot … refuses a request with staged kexts” |
| AMFI 信任 | 没有 | 同上：kext 的 CD hash 必须出现在 payload 的 trust cache 里，否则报签名错误 |
| KPI 符号集（`OSBundleLibraries`） | 未确认 | kext 链接的是 `com.apple.kpi.*`；内核是否导出这些符号集要先查 |
| iOS 内核 SDK / Xcode KEXT target | 未确认 | 文档给的构建方式是 `xcodebuild -sdk iphoneos`，`MACH_O_TYPE = mh_kext_bundle`，`ARCHS = arm64/arm64e` |
| 正确的 `Info.plist` | 纸面值 | 类名 `NativeMaliGpuService` 与代码一致；`IOProviderClass`、匹配规则、bundle id 都未验证（`docs/UNVERIFIED.md`） |
| 暂存配置 | 未写 | UniXNU 配置里的 `[[entry.kext]]` / `[[kext]]`，以及 payload 的 `trust_cache_sha256` |

换句话说：**kext 不是“多一个 plist”，而是多一个 loader、一套签名/信任链和一套
符号解析**。在内核还没有跑过第一条 job 之前，这条路只会增加变量。

## 三、本机实测（2026-10-31）

这些是量过的，不是推测：

| 项 | 结果 |
| --- | --- |
| `xcrun --sdk iphoneos --show-sdk-path` | 失败：`SDK "iphoneos" cannot be located` |
| `xcode-select -p` | `/Library/Developer/CommandLineTools`（只有命令行工具，没有 Xcode） |
| `clang --version` | Apple clang 21.0.0，默认 target 是 `arm64-apple-darwin27.0.0`，不是 `arm64-apple-ios17.0` |
| `UniXNU-merlinx/build/` | 不存在 |
| `*.cpo.json` | 全仓库找不到一份 |
| `UniXNU-merlinx/feeds.toml` | 钉住 `xnu-10063.141.1`，commit `d8b80295118ef25ac3a784134bcf95cd8e88109f`，darwin `23.6.0` |

所以“真实 ABI 编译”在当前环境里一步都走不了：没有 iOS SDK，没有内核对象树，
`build-arm-driver.py` 读取的 `build/xnu/ios-obj/.../IOPlatformExpert.cpo.json`
也不存在。这不是代码问题，是环境问题。

## 四、多 TU 怎么进内核

`build-arm-driver.py` 只编译一个 TU，而 Mali 的驱动有二十多个 `.cpp`。照
`UniXNU-merlinx/scripts/build-native-aes-bindings.py` 的做法：每个 TU 用同一份
`cpo.json` 参数编成 `.o`，再 `xcrun ar rcs libNativeMali.a ...`，然后用
`CONFIG_XNU_EXTRA_LD_ARCHIVES`（`configs/merlinx_defconfig` 里 `NativeAES`、
`MerlinxStubs` 就是这么加的）或 `board.toml` 的 `[kernel] archives` 把归档交给
内核链接。`build-arm-driver.py` 那一步可以当“第一 TU + `-Werror` 门”，但完整的
Mali 驱动还需要一个自己的归档脚本。

## 五、我需要你给的最小输入

按优先级：

1. **选路线**：内置驱动（推荐）还是 kext。
2. 若内置：UniXNU 的**内核构建树**（或至少一份
   `build/xnu/ios-obj/.../IOPlatformExpert.cpo.json`），以及内核源码的
   revision。这是唯一挡在“真实编译”前面的东西。
3. 确认 `docs/OPEN_QUESTIONS.md` 第 5 条的五项 XNU 能力分别在不在；缺哪项，
   对应胶水就继续标 TODO，不猜。
4. 是否允许我改 `UniXNU-merlinx`（子模块、ADT 节点、`board.toml`、
   `build-arm-driver.py`、`KernelConfigTables.cpp`）；不允许的话，我继续只出
   补丁文本。
5. 刷机与日志由谁执行、用哪条通道（串口 / pstore / liveboot）。

## 六、拿到之前不做什么

- 不把 `xnu/Info.plist` 当成已启用；它只是参考文件。
- 不写“看起来能过”的构建脚本；`build-arm-driver.py` 用的是内核自己的参数，
  自造一套只能证明语法。
- 不猜 KPI 符号集、`IOProviderClass` 或匹配规则。
- 不因为没有 kext loader 就退化成用户态直接碰寄存器；ABI 只有 `NativeMali` 一条。
