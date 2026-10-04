# BOSS · 任务9：识别 boot 镜像 / 裸 ramdisk，主界面显示"是/否"

> 需求：用户怕变砖，会先在模拟器或虚拟机上试。这类环境常直接暴露一个
> **ramdisk**（不是完整 boot 镜像）。管理器要能分辨这两种，
> 并在主界面显示 ramdisk 状态——有就"是"，没有就"否"。

## 1. 为什么必须分这两种

| | 真机 | 模拟器 / 虚拟机 / 容器化 Android |
|---|---|---|
| 操作对象 | 完整分区镜像（`init_boot.img`） | 常是裸 ramdisk（cpio，可能压缩） |
| 有没有 boot 头 | 有 | **没有** |
| veritpath inject | 直接改 ramdisk 段 | **不适用** |

混为一谈的后果：拿裸 ramdisk 去 `inject`，报错指向"镜像解析失败"，
用户会以为是镜像坏了，实际是**类型不对**。

## 2. 判定依据：文件头 magic

`core/RamdiskProbe.kt`：

| magic | 判定 | 有 ramdisk？ |
|---|---|---|
| `ANDROID!` | boot 镜像 | 是（段里含） |
| `VNDRBOOT` | vendor_boot 镜像 | 是 |
| `070701` / `070702` | 裸 cpio | 是（本身就是） |
| `1f 8b` | gzip → **解压确认** | 是 cpio 则"是" |
| `04 22 4d 18` / `02 21 4c 18` | lz4（含 legacy） | 是 |
| `fd 37 7a 58 5a 00` | xz | 是 |
| `42 5a 68` | bzip2 | 是 |
| 都不匹配 | UNKNOWN | **否** |

### ⚠️ gzip 一定要真的解压再判

只看首字节会把"压缩的裸 ramdisk"误判，但更关键的是反面：
**boot 镜像的 ramdisk 段也常是 gzip**，外层仍是 boot 镜像。
所以 gzip 必须解压后看头 6 字节是不是 `070701`。

实测（含一个"gzip 但内容不是 cpio"的反例）：

```
boot.img        -> BOOT         是（boot 镜像）
vendor_boot.img -> VENDOR_BOOT  是（vendor_boot 镜像）
raw.cpio        -> RAW_CPIO     是（裸 cpio）
gz_cpio.gz      -> GZIP_CPIO    是（gzip 压缩的 ramdisk）
gz_other.gz     -> UNKNOWN      否     ← 解压后不是 cpio，不能硬说它是
lz4.cpio        -> LZ4_CPIO     是
lz4legacy.cpio  -> LZ4_CPIO     是
xz.cpio         -> XZ_CPIO      是
bz2.cpio        -> BZIP2_CPIO   是
junk.bin        -> UNKNOWN      否
nosuch.img      -> MISSING      否
```

用 veritpath 自己的产物复核过：
`init_boot.img` → BOOT；`unpack` 出来的 `ramdisk.cpio` → RAW_CPIO。

## 3. 主界面显示

`HomeScreen` 里新增一行，按用户要求的措辞：

```
ramdisk        是（boot 镜像）        ← warn=false
ramdisk        否                     ← warn=true（红色）
  └ 位置       /data/local/tmp/boot.img
  └ 大小       8192 KB
```

类型与路径附在后面——裸 ramdisk 和 boot 镜像处理路径不同，得让人看见是哪一种。

## 4. 探测独立于 BOSS 是否运行

`BossRepository.status()` 里 ramdisk 探测**不依赖 bossd**：

> 虚拟机上 BOSS 没装，但环境照样可能暴露 ramdisk——
> 那正是"先在虚拟机里试、怕变砖"的用户要看的东西。
> 要是挂到"bossd 起来了才探测"，这个功能在最该有用的场景反而不显示。

候选位置（`RamdiskProbe.CANDIDATES`）覆盖 `/data/local/tmp`、`/data/adb`、
`/sdcard`、`/tmp` 下的 `ramdisk.cpio(.gz)`、`boot.img`、`init_boot.img` 等。

⚠️ 判定用"第一个**确实含 ramdisk**的"，不是"第一个存在的"——
目录里可能有空的 `boot.img` 占位文件，那应该继续找，而不是报"找到了"。

## 5. 读不到时走 root 通道

App 对 `/data/local/tmp` 这类路径通常没有读权限，所以：

- 能直接读 → `java.io.File`
- 读不到 → `RootShell.shSync()`，自动挑通道（第三方 su **或** BOSS daemon）

⚠️ 这里踩过一个设计坑：一开始只认第三方 su 的 `cachedSu`。
**装了 BOSS 的机器上 `cachedSu` 是 null**（那条路从没走过），
结果"装了 BOSS 反而读不到文件头"。改成两条通道都试。

## 6. 补充：为什么在虚拟机上"找不到"（真因不是路径没列全）

用户报：ramdisk 就在 `/dev/block/platform/host/by-name/`，App 却显示"否"。

**真因是一个致命的 size 判据，不是候选路径缺失：**

```kotlin
val size = sizeOf(path) ?: return null
if (size <= 0) return@withContext null     // ← 块设备被这里全灭
```

**块设备的 `File.length()` 一律返回 0：**

```
/dev/zero     isFile=false   size=0
/dev/null     isFile=false   size=0
/dev/urandom  isFile=false   size=0
```

`/dev/block/**` 下全是块设备节点，size 一律 0 → **全部被无声跳过**。
就算路径列对了，也永远显示"否"。

### 修法

| 问题 | 修法 |
|---|---|
| 块设备 size 恒为 0 | 块设备走 `blockdev --getsize64` / `/sys/class/block/<n>/size×512`，**且不做 <=0 过滤** |
| 候选只有普通文件 | 新增 by-name 块设备枚举 |
| 平台名写死 | **不写死**，运行时枚举 1~2 层 |

`File` 空文件仍按 0 过滤——那个语义是对的，只对块设备放开。

### by-name 目录是**两级不固定**的，不能硬编码 `host`

```
/dev/block/by-name                      ← 少数设备
/dev/block/platform/<soc>/by-name       ← 常见（host 只是某一台上的名字）
/dev/block/platform/<soc>/<x>/by-name   ← 有些多一层
```

真机上是 soc 厂商名（如 `soc/1d84000.ufshc`），写死 `host` 在真机上必然失效。

实测三种布局（假 shell 注入）：

```
虚拟机(host/ramdisk)  枚举到 3 个: ['ramdisk','boot','vendor_boot']
                      首个(优先) = /dev/block/platform/host/by-name/ramdisk
两层soc               枚举到 2 个: ['init_boot','boot']
直连by-name           枚举到 1 个: ['ramdisk']
```

名称优先级 `ramdisk > init_boot > boot > vendor_boot > recovery` 生效。

### 顺带修：本机安装页有**同一个盲区**

`LocalInstallRepository.findPartition()` 原来也只查 `/dev/block/by-name`，
在虚拟机上永远"未探测到分区"。已改为**复用 `RamdiskProbe.enumerateBlockDevices()`**
——两边共用一套枚举，免得一处修了另一处还是瞎的。

### gzip 块设备分支

块设备没有 `FileInputStream`，gzip 解压确认改走 shell 管道：

```
dd if=<dev> bs=1 count=512 | gzip -dc | head -c 6 | od -An -tx1
```

取 512 字节（只给 8 字节 inflate 不出 6 个字节；又不至于读整个分区）。
实测：`gzip(cpio)` → GZIP_CPIO；`gzip(非cpio)` → **UNKNOWN（不误报）**。

### 找不到时把扫过的地方列出来

否则用户只能报"显示否"，而"扫过哪些"才是定位的关键信息。
主界面现在会列出扫描过的路径（节选 4 条）。

## 6.5 ⚠️ 修的过程中引爆了一个更隐蔽的坑：Kotlin 块注释**可嵌套**

修完探测逻辑后编译炸出几十条错：

```
RamdiskProbe.kt:167:6  Syntax error: Missing '}'
RamdiskProbe.kt:316:1  Syntax error: Unclosed comment
BossRepository.kt:44   Unresolved reference 'find'
LocalInstallViewModel.kt:30  Unresolved reference 'PartitionInfo'
...（几十条）
```

看着像"整个模块坏了"，**实际只错了一行注释**。

### 真因

注释正文里写了路径：

```kotlin
 * 因为模拟器/虚拟机把 ramdisk 暴露成 `/dev/block/platform/*/by-name/*`
```

`/dev/block/platform/*/by-name/*` 里有两个 `/*`、**零个** `*/`
→ 各开一层嵌套注释 → 一直吞到文件尾。

同样中招的还有 `于是 /dev/block/** 下面`（`/**` 里的 `/*` 也开层）。

### 这个坑的三个特征

1. **报错位置指向文件末尾**，离真因（第 173 行）很远
2. 引发**几十条 Unresolved reference** 连锁报错——因为类体被注释吞掉，
   里面的类型全找不到了
3. 我那个"数花括号"的静态检查**测不出来**：它连注释里的括号一起数，
   开头多一个 `{` 结尾多一个 `}` 正好抵消，结果是"平衡"的

### 修法 + 守卫

- 注释里的路径改成 `<soc>` / `<name>` 占位写法，不出现 `/*`
- 新增 `tools/kotlin_comment_scan.py`：正确扫描块注释嵌套
  （**只在注释外解析引号**）
- 自检 3.8 节接入，不平衡直接 FAIL

### ⚠️ 扫描器第一版自己也是错的（假阳性）

它在注释**内部**也解析引号，于是上游文件里一句
`Release the buffer holding the last command's output.`
——那个撇号被当成字符字面量起点，一路吞掉后面的 `*/`，
**一个完全正常的上游文件被报成"未闭合"**。

修法：进入注释后**只**看 `/*` 与 `*/`，不再解析引号。

### 反证（两头都验过）

| 输入 | 结果 |
|---|---|
| 注入两种真实 bug | FAIL，指出第 169 行 |
| 还原 | ok，21 个文件全平衡 |
| 上游 Veritpath.java | ok，**不误报** |

> 这是本仓**第四次**栽在"注释里的字面量"：
> `?attr/colorControlNormal`、`Veritpath.analyze(...)`、
> `write Veritpath.run(...)`、现在是 `/*`。
> **写检查脚本时，你自己的注释也是输入的一部分。**

## 6.6 ⚠️ 我自己改脚本时误删了两个函数（readHead / hexToBytes）

编译报：

```
RamdiskProbe.kt:217:20  Unresolved reference 'readHead'
RamdiskProbe.kt:309:17  Unresolved reference 'hexToBytes'
```

**是我上一轮用 Python 按字符串索引切片改文件时，把这两个 private 函数
整块删掉了。** 函数已补回。

### 为什么之前的检查全都没抓到

| 检查 | 为什么漏了 |
|---|---|
| 数花括号 | 删的是**一整块平衡的括号**——开头少一个 `{`、结尾也少一个 `}`，计数仍是 0 |
| 块注释扫描 | 注释是平衡的，跟函数删没删无关 |
| 调用/定义核对 | 只查跨文件（`repo.xxx`、`vm.xxx`），**不查同文件内的 private 成员** |

> 这是同一个教训的第二次：
> **结构性检查（括号/引号平衡）给的是虚假的安全感。**
> 平衡不等于正确。

### 试过做通用"调用但未定义"检查，放弃了

第一版扫出 **200 多条全噪声**：

```
MainActivity.kt 调用了未定义的 Text()
Theme.kt 调用了未定义的 Color()
```

原因：Compose 大量用**通配导入**（`androidx.compose.material3.*`），
无法把 `Text` / `Column` 这类名字解析回导入。
**没有编译器就做不出可靠的通用检查**——硬做只会更假。

### 最终做法：只守关键符号的存在性

`tools/kotlin_symbols_scan.py`，一张手写的表
（每个文件不容丢失的关键 `fun` / `val` / `data class` / `enum class`）。

- **零噪声**（实测：误删 → 精确指出缺 `fun readHead`、`fun hexToBytes`；还原 → 17 个文件齐全）
- 正好覆盖"被脚本误删"这个真实故障
- 自检 3.9 节接入

代价是要手工维护表：新增关键函数时补一条。

### ⚠️ 补回函数时，我把同一个注释 bug 又写了一遍

补 `readHead` 时把注释原样抄回来：

```kotlin
 * ⚠️ 块设备**一律走 root**：App 对 /dev/block/** 既没有读权限，
```

`/dev/block/**` 里的 `/*` 又开了一层嵌套注释——**同一个坑，第二次踩**。
是块注释扫描器（3.8 节）当场抓出来的，不是编译报的。

> 教训：修过一次不代表不会再犯，**自动化守卫比记忆可靠**。
> 这个坑在本仓已经触发两次，只有扫描器挡住了第二次。

### 附带修正

表里 `BossViewModel` 的 `verify` 一开始写成 `fun verify`，
实际它是 `val verify: StateFlow<VerifyReport?>`（HomeScreen 用
`collectAsStateWithLifecycle()` 订阅，不是调用）。写成 `fun` 会误报。

## 7. 验证状态

| 项 | 状态 |
|---|---|
| magic 判定逻辑（含 gzip 反例） | ✅ 沙盒实测 10 个样本 |
| veritpath 真实产物复核 | ✅ `init_boot.img`→BOOT、`ramdisk.cpio`→RAW_CPIO |
| Kotlin 静态检查（import、括号） | ✅ 通过 |
| C 侧回归 | ✅ 全绿 |
| 枚举逻辑（3 种 by-name 布局） | ✅ 假 shell 注入实测 |
| 块设备 size=0 分支（旧逻辑 vs 新逻辑） | ✅ 复刻验证 |
| gzip 块设备解压确认（含非 cpio 反例） | ✅ 实测 |
| **真机 / 模拟器上探测** | ❌ **未验** |
| **Gradle 编译** | ❌ 未验（沙盒无 SDK） |

⚠️ 候选路径是**猜的**。各模拟器/虚拟机实际暴露的位置差别很大，
真跑一次很可能一个都命中不了——那就得让用户手动选文件，
`RamdiskProbe.classify(path)` 已经支持对任意路径判定。
