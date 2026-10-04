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

## 6. 验证状态

| 项 | 状态 |
|---|---|
| magic 判定逻辑（含 gzip 反例） | ✅ 沙盒实测 10 个样本 |
| veritpath 真实产物复核 | ✅ `init_boot.img`→BOOT、`ramdisk.cpio`→RAW_CPIO |
| Kotlin 静态检查（import、括号） | ✅ 通过 |
| C 侧回归 | ✅ 全绿 |
| **真机 / 模拟器上探测** | ❌ **未验** |
| **Gradle 编译** | ❌ 未验（沙盒无 SDK） |

⚠️ 候选路径是**猜的**。各模拟器/虚拟机实际暴露的位置差别很大，
真跑一次很可能一个都命中不了——那就得让用户手动选文件，
`RamdiskProbe.classify(path)` 已经支持对任意路径判定。
