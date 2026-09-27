# BOSS · 整合与出包：payload、客户端、veritpath 三者的关系

> 这份回答一个问题：**"编译全绿了，接下来是不是得把它们和客户端整合起来？"**
>
> 结论分两半：
> - **veritpath 不用整合进客户端**——它根本不是运行时的一部分。
> - **出包链路确实要整合**——镜像侧产物与客户端 APK 必须进同一个 release。
>
> 顺带记下这次整合里查出的三个真实缺口（第 0 节），它们都是"看着绿、其实没好"。

---

## 0. 先说结论里的三个"其实还没好"

"编译全绿"绿的是 **C 侧**：`make test`、协议布局断言、四套上游回归、任务6 三套自检。
**客户端 APK 一次都没编译过**——沙盒里没有 Android SDK、Gradle、kotlinc。

所以这次做的是**静态交叉检查**（符号引用、依赖声明、跨 package import），查出三个
必然会让第一次真编译红掉的缺口：

### ① 全量包里根本没有 Gradle 构建骨架（最严重）

合并时只复制了 `app/`，而 `settings.gradle.kts`、根 `build.gradle.kts`、
`gradle.properties`、`gradle/libs.versions.toml` 都在**仓根**，一个都没跟过来。

于是全量包里的 App **连 `./gradlew` 都跑不起来**——不是编译报错，是压根没有构建系统。

> 这个坑的形状和 `.gitignore` 那条老坑一模一样：本地看不出来，因为文件一直躺在
> 自己的工作区里；到了 CI 才是 "No such file or directory"。

已补齐，并在 `.gitignore` 里加了 Gradle 那一组（注意与仓根 `build/*` 不冲突：
那个是 BOSS 本体的产物目录，这里是 Gradle 的）。

### ② `Icons` 没有 import，而且缺图标依赖

`MainActivity.kt` 里用了 `Icons.Default.Home / Extension / Security / VisibilityOff`，
但：

- **没有 import**。`Icons` 属于 `androidx.compose.material.icons`，
  而 `import androidx.compose.material3.*` **不会**把它带进来。
  漏了它的表现是 `Unresolved reference: Icons`，而且只在这一处。
- **缺依赖**。Home 在 `material-icons-core` 里，但 Extension / Security /
  VisibilityOff 只在 `material-icons-extended`。
  挂错的表现是 `Unresolved reference: Extension`，报错**不会**提示"换 extended 就好"，
  只会让人以为图标名写错了。

已修：加 `import androidx.compose.material.icons.Icons` + 四个 `filled.*`，
依赖挂 `material-icons-extended`。

> 代价：extended 是全量图标集，APK 会大几百 KB。
> 想瘦身就把这四个换成 core 里确实存在的图标，然后改挂 core。

### ③ release 里不出 APK

`release.yml` 只出 `boss-android`（NDK 四 ABI）、`boss-payload.zip`、`boss-linux`。
**没有 APK**——用户刷完镜像拿不到客户端。

已加 `apk` job。两个决定：

- **没有 gradlew wrapper**（wrapper 的 jar 是二进制，我这边生成不了）。
  所以 CI 显式下载固定版本 Gradle（`GRADLE_VER=8.11.1`），而不是依赖 runner 预装的
  gradle——预装版本与 AGP 要求不符时报错是 "Minimum supported Gradle version is ..."，
  而它不会告诉你该装哪个。**改 `libs.versions.toml` 里的 `agp` 时记得同步这里。**
- **出 Debug 包而不是 Release**。Release 需要签名密钥，而仓库里不该放密钥。
  要发正式包时，在 CI 里配 secrets 并在 `app/build.gradle.kts` 加 `signingConfig`，
  **别把 keystore 提交进来**。

⚠️ **这个 job 我一次都没跑过**（沙盒没有 SDK）。第一次跑大概率要调 Gradle/AGP 版本。
它的失败不算意外，别靠"放宽校验"让它绿。

---

## 1. veritpath 不需要整合进客户端

先把这个疑虑消掉。三者的关系是**流水线**，不是"拼在一起"：

```
   本仓                     1 号的仓               产物
┌──────────┐            ┌───────────┐        ┌──────────────┐
│ src/*.c  │  make      │ veritpath │ inject │ init_boot    │
│ payload/ │ ─────────► │  (PC 端)  │ ─────► │ .img         │
└──────────┘  payload   └───────────┘        └──────┬───────┘
                                                    │ fastboot flash
                                                    ▼
┌──────────┐                                 ┌──────────────┐
│  app/    │  gradle ────► boss-app.apk ──adb install ──►│  手机   │
└──────────┘                                 └──────────────┘
```

- **veritpath 是 PC 端工具**：分析 boot 镜像、注入 payload、产出可刷的 img。
  它在**构建期**跑一次，之后和手机没有任何关系。
- **客户端是手机上的普通 APK**：运行时只跟 `bossd` 说话（抽象命名空间 socket）。

两者**没有运行时交集**，硬塞进同一个产物只会让 payload 变大、并引入不该有的东西。

⚠️ 别被 `payload/manifest.json` 里那个 `/veritpath/boss` 误导——那是镜像内的
**冗余放置路径**（`required: false`），跟工具本身无关，同名纯属巧合。

---

## 2. 真正要整合的是出包链路

一个 release 里应该有四样东西，缺一样用户就用不起来：

| 产物 | 谁产 | 用户怎么用它 |
|---|---|---|
| `boss-android-*`（四 ABI） | `build/build-ndk.sh` | 进 payload，刷进 init_boot |
| `boss-payload.zip` | 同上 | 喂给 veritpath 做注入 |
| **`boss-app-debug.apk`** | `gradle :app:assembleDebug` | **adb install，手机上打开** |
| `boss-linux-*`（静态） | `make static` | 调试用 |

**APK 不进 payload。** 这一点是硬要求，不是懒：

> BOSS 与 Magisk 定位差异的根本就是**不往 `/system` 落任何东西**。
> 把 APK 预置进镜像就要写系统分区，直接违反这条。
> 所以客户端必须是**用户自己装的普通 APK**——这也正好，App 本来就不申请任何权限。

### 刷机与使用顺序（这个顺序本身就是设计的一部分）

```
1. veritpath inject  →  init_boot.veritpath.img
2. fastboot flash init_boot
3. 开机 → ls -l /proc/1/exe        ← 必须指向 boss，否则后面全是空的
4. adb install boss-app-debug.apk
5. 打开 App → 首次连接 daemon 时抢注 manager.uid
```

**第 3 步和第 5 步的先后不能颠倒**：manager 抢注的前提是 daemon 已经跑起来，
而 daemon 靠 init 接管拉起来。`/proc/1/exe` 没指向 boss 时打开 App，
表现为"所有功能都显示被拒绝"——而真因是**没人拉它起来**。

---

## 3. 整合后的验证边界（诚实版）

| 项 | 状态 |
|---|---|
| C 侧 `make test` + 上游四套回归 | ✅ 已验（28 / 16 / 17 / 8，无回归） |
| 协议布局断言 + IPC 端到端 | ✅ 已验（10 项 × 两种身份） |
| manager 与弹窗端到端 | ✅ 已验（14 项） |
| CLI 输出契约 | ✅ 已验（18 项） |
| **APK 能否编译** | ❌ **未验**（沙盒无 SDK；静态检查已过，但没真编译过） |
| **Gradle/AGP 版本是否匹配** | ❌ 未验 |
| **JNI 在真机上能否加载 .so** | ❌ 未验 |
| **真机跑通全流程** | ❌ 未验 |

沙盒能做的静态检查做到底了（符号、依赖、跨包 import、ViewModel 方法齐全性），
**剩下那四行只能靠有 SDK 的机器**。别把"静态检查过了"当成"APK 编得出来"。

---

## 4. 下一步建议

1. **先在有 Android SDK 的机器上跑一次** `gradle :app:assembleDebug`，
   把 Gradle/AGP/Compose 版本定下来，回写 `libs.versions.toml` 与 `release.yml` 的
   `GRADLE_VER`。这一步大概率要调，是正常的。
2. **本地 `adb install` 一次**，确认 App 能起来、能连上 daemon——
   这一步能同时验掉"JNI 加载"和"抢注"两个未验项。
3. 再往后才是真机验收清单（`docs/TASK6-BOSS客户端-解析与交付.md` 第 6 节）。

---

## 4.5 第一次真编译撞上的坑：工具链是一条链，不是一个数字

实际跑 `gradle :app:assembleDebug` 时，在 `checkDebugAarMetadata` 挂了，
报 **29 个问题**，形如：

```
依赖项 "androidx.compose.material3:material3-ripple-android:1.5.0-alpha27"
  需要依赖它的库/应用针对 Android API 37 或更高版本编译
  需要 Android Gradle 插件 9.1.0 或更高版本
:app 目前是针对 android-36 编译的，AGP 是 8.9.0
```

同时 Gradle 还友好地建议：`请添加 android.suppressUnsupportedCompileSdk=36`。

**这条建议是陷阱，别听。** 它压的是另一件事（"AGP 只测试到 compileSdk 35"的提醒），
与这 29 个问题无关。这 29 个是 **AAR 元数据硬校验**——不是警告、不是 lint，
加了那行之后警告没了、29 个问题一个不少，还少了一条线索。

### 真实成因：一条四节的版本链

```
Compose BOM 2026.08.00 把 ui/foundation 锁到 1.12.0
        └─► 1.12.0 的 AAR 声明 "compileSdk ≥ 37 + AGP ≥ 9.1.0"
                └─► compileSdk 37 需要 AGP ≥ 9.1.1（AGP 9.0.x 最高只收 36）
                        └─► AGP 9.1.1 需要 Gradle ≥ 9.3.1
```

**动一个必须一起动。** 所以不是"把 compileSdk 改成 37"就完了——
只改那一个数字会得到 "Minimum supported Gradle version is ..." 之类的下一层报错。

最终取值（已写进 `gradle/libs.versions.toml`）：

| 项 | 原 | 现 | 为什么 |
|---|---|---|---|
| `agp` | 8.9.0 | **9.1.1** | 官方声明支持 API 37.0 及以下的版本 |
| `kotlin` | 2.2.0 | **2.3.0** | AGP 9.x 内置 KGP；Kotlin 2.3 要求 AGP ≥ 9.0.28，9.1.1 安全 |
| Gradle（CI） | 8.11.1 | **9.3.1** | AGP 9.1.1 的硬下限 |
| `compileSdk` | 36 | **37** | 被依赖逼的 |
| `targetSdk` | 36 | **36（不动）** | 见下 |

**targetSdk 刻意不跟。** 这三者是独立的：compileSdk 管"能调哪些新 API"，
targetSdk 管"采不采用新运行时行为"。Android 16 的 edge-to-edge 与预测式返回
在 36 上已经是强制的，够用了；推到 37 会引入一批**我们一行都没验过**的行为变更。

### 本地要做的两件事（CI 已经写好了，本地得自己做）

```bash
# 1) 装 android-37 平台 + Build Tools 36.0.0（不装的话 AGP 连目标都找不到）
sdkmanager --install "platforms;android-37" "build-tools;36.0.0"

# 2) Gradle 升到 9.3.1+（仓库没有 gradlew wrapper，wrapper jar 是二进制）
```

⚠️ 万一报 `Failed to find target with hash string 'android-37'`：
某些 SDK 版本里 API 37 只以次要版本形式发行（`android-37.0`），
这时要在 `android {}` 里补一行 `compileSdkMinor = 0`（要求 AGP ≥ 9.1.0）。
我们没默认写它——多数环境下写了反而找不到目标。

### 先确认你编的是不是最新那份（这一步最常撞上）

报错里那句 `This build currently uses Android Gradle plugin 8.9.0` 是**关键线索**——
它说的是**当前这份代码里**的 AGP，不是你机器上的。仓库里已经是 9.1.1，
所以出现 8.9.0 基本等于：**推上去的还是旧包（或本地没拉新）**。

先在仓库里自查，比看那面报错墙快得多：

```bash
bash tools/check_android_toolchain.sh
```

它会把 AGP / Kotlin / BOM / material3 / compileSdk / targetSdk 原样打出来，
对着下限逐条校验，版本不对当场指出是哪一行。不需要 Android SDK 也能跑。

```
agp        = 9.1.1        ← 如果是 8.9.0，说明推的是旧包
compileSdk = 37
targetSdk  = 36
结果：PASS
```

CI 的 apk job 里也有同一段自检，而且放在**编译之前**——
版本不对就在那一步停掉，不用等一分钟后看那 29 条。

### 如果装不了 android-37，还有一条退路

把整条 Compose 栈降回不要求 37 的组合：

| 项 | 降为 |
|---|---|
| `composeBom` | `2026.06.00`（锁 Compose 1.11.x，不要求 compileSdk 37） |
| `material3` | `1.4.0` 稳定版 |
| `compileSdk` | `36` |
| `agp` | `8.9.1`+（够用了，Gradle 8.11.1 即可） |

⚠️ 真正的代价在代码里，不只是版本号：**Material 3 Expressive 会整个没了。**
`MaterialExpressiveTheme` / `MotionScheme` / `LoadingIndicator` 全部不可用，
`ui/theme/Theme.kt` 得退回普通 `MaterialTheme`，`ui/MainActivity.kt` 里的
`LoadingIndicator` 也得换成 `LinearProgressIndicator`。

⚠️ 另外别以为"只降 BOM、留着 material3 alpha27"能两全——
alpha27 本身就依赖 Compose 1.12.0，降了 BOM 会和它打起来，
症状是依赖冲突而不是 AAR 校验失败，更难查。

要不要吃这个代价是产品决策，不是技术决策——所以**先试升工具链，升不动再谈降级**。
真要走这条路，把上面那张表的四个值改掉之后说一声，我把 `Theme.kt` 一起改过去。

---

## 5. 三条别做错的事

- **别把 APK 塞进 payload。** 写系统分区违反无修改原则，这是 BOSS 的定位底线。
- **别把 keystore 提交进仓库。** 要签正式包就在 CI 里用 secrets。
- **别为了 CI 变绿放宽 Gradle 校验。** 放宽了就等于把"版本不匹配"藏起来，
  它会在真机上以更难查的形式冒出来。
