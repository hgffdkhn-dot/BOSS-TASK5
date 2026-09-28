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

### ⓪ launcher 图标用了 AppCompat 的属性（最近这次的报错）

```
Android资源链接失败
com.boss.manager.app-main-41:/drawable/ic_boss.xml:12:
  错误：找不到资源 attr/colorControlNormal
```

`?attr/colorControlNormal` 是 **AppCompat** 定义的属性。BOSS 是纯 Compose 项目，
**不依赖 appcompat**，所以它在我们自己的包名下根本不存在 —— 链接直接失败。

修的时候要同时想清楚两条，第二条更要命：

1. **构建期**：加 `android:` 前缀（`?android:attr/colorControlNormal`，框架属性，
   API 21 有）能编过。但——
2. **运行期**：**launcher 图标是在 launcher 进程里加载的，用 launcher 的 theme，
   不是我们的。** 任何 `?attr/` 在那里解析成什么都不由我们决定，
   同一份图标在不同 launcher 上颜色会不一样，甚至解析失败。
   → **launcher 图标必须写死颜色，没有例外。**

顺带修了一个没报错但不对的地方：原来直接把 24dp 矢量图当 `android:icon`，
那样在 Android 8+ 上**不会被遮罩裁切**，图标"裸"着显示，
在别的图标都是统一形状的桌面上很扎眼——对主打低痕迹的产品是反效果。
现在改成标准自适应图标：

```
drawable/ic_launcher_foreground.xml    108dp viewport，内容收在中心 66dp 安全区
drawable/ic_launcher_background.xml    108dp 满铺
mipmap-anydpi-v26/ic_launcher.xml      adaptive-icon
mipmap-anydpi-v26/ic_launcher_round.xml
```

minSdk 就是 26，所以**不需要** PNG 回退，`mipmap-anydpi-v26` 已覆盖全部受支持设备。

配色取自 `Theme.kt` 里 BossDark 的回退色（`surface #FF101415` / `primary #FF9ECAFF`），
低饱和中性色，不给 BOSS 造一个能被认出来的招牌色。

**自检脚本加了 3.7 节专门守这个**：扫 `res/` 里的 `?attr/`，
没在 `res/values` 声明就报 FAIL；并顺带检查 `android:icon` 是否指向 `@mipmap`
（指向 `@drawable` 说明不是自适应图标）。

> 实现细节：扫描前先剥掉 XML 注释。注释里把 `?attr/colorControlNormal`
> 当反面例子写着，不剥掉就永远误报——已实测能抓真问题、不误报注释。

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

### 第三个坑：AGP 9 内置了 Kotlin，`kotlin.android` 不能再声明

```
InvalidPluginException: An exception occurred applying plugin request
    [id: 'org.jetbrains.kotlin.android', version: '2.3.0']
→ The 'org.jetbrains.kotlin.android' plugin is no longer required
  for Kotlin support since AGP 9.0.
```

**AGP 9.0 起 Kotlin 编译内置进 AGP**，官方迁移步骤第一步就是把
`org.jetbrains.kotlin.android` 从**三处**删掉：版本目录、根脚本、模块脚本。

这个报错的坑在于措辞：它说"应用插件时异常"，**完全不提"这个插件不该出现"**。
只看第一行会往"2.3.0 版本不对"上想，然后去降 Kotlin——方向错了。

改完之后的三处形态：

| 位置 | 内容 |
|---|---|
| `gradle/libs.versions.toml` | `[plugins]` 里**只有** `kotlin-compose`（Compose 编译器），没有 `kotlin-android` |
| 根 `build.gradle.kts` | `buildscript` 里显式拉 `kotlin-gradle-plugin:2.3.0` |
| `app/build.gradle.kts` | 只留 `kotlin.compose`；编译器选项搬到顶层 `kotlin { compilerOptions {} }` |

两个连带要点：

1. **`kotlin` 这个版本号现在只喂给 Compose 编译器插件**——Compose 编译器随 Kotlin 一起发布，
   两者版本号必须一致。真正的 Kotlin 编译器版本靠根脚本 `buildscript` 里的
   `classpath("org.jetbrains.kotlin:kotlin-gradle-plugin:2.3.0")` 指定——
   因为 **buildscript 块里读不到版本目录**，那串数字得手写，改版本时两处都要改。
   （自检脚本会比对这两处是否同号。）
2. **`android { kotlinOptions {} }` 是旧写法**，AGP 9 内置 Kotlin 下要搬到顶层
   `kotlin { compilerOptions { jvmTarget / optIn } }`。
   `optIn` 那一项不能省：Expressive 的 API 还在 alpha，不开全局 opt-in
   就得每个文件手写 `@OptIn`，漏一个就编译失败。

⚠️ **别用 `android.builtInKotlin=false` 绕。** 那是兼容退路（关掉内置 Kotlin、回到手动声明
kotlin.android），官方说会在 AGP 10 移除。开了等于把刚迁好的东西退回旧模型，
而且会让刚删掉的插件重新变成"必须"——自相矛盾。

自检脚本加了 3.5 节专门查这一组：

```
== 3.5) AGP 9 的内置 Kotlin ==
  ok   三处构建脚本里都没有 kotlin.android
  ok   Compose 编译器插件在（Kotlin 版本只喂给它）
  ok   根脚本 KGP(2.3.0) == 版本目录 kotlin(2.3.0)（两处必须同号）
```

### 又一个坑：`sdkmanager` 装不到 android-37

CI 上真跑出来的第二条：

```
yes: standard output: Broken pipe
Warning: Failed to find package 'platforms;android-37'
Error: Process completed with exit code 1
```

**两个信息要分开看：**

- `Broken pipe` 是**噪声**，不是错误。它来自 `yes | sdkmanager --licenses`——
  sdkmanager 提前退出了，`yes` 还在往关闭的管道里写。用 `yes 2>/dev/null` 收掉即可。
- `Failed to find package 'platforms;android-37'` 是**真问题**：
  这个 runner 的 sdkmanager 渠道里**没有 android-37 这个包**。

⚠️ 关键判断：**这不代表 API 37 不存在，只代表这个渠道没发到它。**
所以不能据此断定"必须降级"——得让编译器自己说话。

CI 上的处理分两步：

1. **安装步骤改成"尽力而为，不阻断"**（`continue-on-error: true`）。
   允许它失败，把已装的东西打出来，交给编译阶段给结论。
   装不上就在这里挂掉，等于真正的编译一次都没跑，白等。
2. **构建步骤带回退**：先试 37；失败且确认平台确实不存在时，
   把 `compileSdk` 临时改成 36 再试一次。

⚠️ **预期 36 也会失败**——Compose 1.12.0 的 AAR 硬校验同样要求 compileSdk ≥ 37。
这么做的目的不是让 36 成功，而是**用两次失败把原因分开**：
- 37 报"找不到平台"、36 报"AAR 元数据" → 平台缺失
- 两条都报"AAR 元数据" → 版本链不对（AGP/Gradle/BOM）

对着一条报错猜，是这类问题最大的时间黑洞。

### 决策：**选方案 1——换镜像/换工具，保住 API 37 与 Expressive**

（方案 2 是降级到 compileSdk 36 + material3 1.4.0，会丢掉整个 Material 3 Expressive。
已决定不走那条。）

落地做了三件事，按重要性排：

**① 下载官方 latest 通道的 cmdline-tools——这是真正解决问题的一步**

```yaml
curl -sSLO https://dl.google.com/android/repository/commandlinetools-linux-13114758_latest.zip
unzip -q commandlinetools-linux-*_latest.zip
mv cmdline-tools "$SDK/cmdline-tools/latest"    # 路径必须是这个布局
```

> ⚠️ 很多人以为"换镜像"指的是换 OS 版本——**不是**。
> android-37 能不能装到，取决于 **sdkmanager（cmdline-tools）的版本与渠道**，
> 跟 Ubuntu 是 22.04 还是 24.04 无关。镜像里预装的 sdkmanager 版本太旧，
> 渠道里就没有这个包。换成 latest 通道的那一份才有机会看到它。
>
> 另外 sdkmanager 必须在 `$SDK/cmdline-tools/latest/bin/` 这个布局下，
> 直接解压出来用会报 `Could not determine SDK root`。

**② runner 镜像钉死为 `ubuntu-24.04`，不用 `ubuntu-latest`**

跟 android-37 无关，是为了**确定性**：`ubuntu-latest` 会随时间漂移，
自带的 SDK 版本跟着变，某天突然红都不知道为什么。钉死后，
升级镜像是一次**有意的提交**，不是意外。

**③ 加 `android.builder.sdkDownload=true` 兜底**

让 AGP 缺平台时自己去补，而不是直接报
"Failed to find target with hash string 'android-37'"。多一层机会，不影响本地。

### 顺带把"猜"改成"看"

新增一个**探测步骤**，在装之前先把渠道里有什么列出来：

```
=== 可用 platforms（grep 35 以上）===
    platforms;android-36 | 1 | Android SDK Platform 36
    platforms;android-37 | 1 | Android SDK Platform 37
```

"装不到"和"渠道里根本没有"长得一模一样，只有 `--list` 能把它们分开。
先看清再决定，比装失败后猜快得多。

### 自检脚本自己报了假警报（FAIL=1，但代码是好的）

CI 上自检输出：

```
FAIL  ?attr/colorControlNormal 未声明
ok    android:icon 指向 @mipmap（自适应图标）
```

但仓库里**根本没有**这个引用——它在 `ic_launcher_foreground.xml` 的
**注释**里，是我拿它当反面例子写的。

成因：扫描前要用 `perl` 剥掉 XML 注释，而 **runner 上没有 perl**，
`strip_comments` 退化成了 `cat`，于是把注释扫了进去。

> ⚠️ 这是同一个坑的**第二次**：上一次是 `gradle.properties` 那条——
> 注释里解释了"为什么别写 suppressUnsupportedCompileSdk"，
> 直接 grep 就命中注释，永远 warn。
> **教训：写检查脚本时，你自己的注释也是输入的一部分。**

两层修法，缺一不可：

1. **注释里不再出现可被匹配的字面量**。改成 `?attr/` + "AppCompat 定义的颜色属性"
   （名字不写出来），这样任何扫描都匹配不到。
2. **剥注释不依赖单一工具**：`python3`（runner 一定有）→ `perl` →
   **两者都没有就跳过这项检查并说明**。

第 2 条的收尾原则值得单独说一句：

> **剥不掉就别下结论**。宁可少查一项，也不要报一个假 FAIL。
> 假 FAIL 的代价远大于漏查——它会让人去改本来正确的代码。

已实测三个分支：

| 场景 | 结果 |
|---|---|
| 无 python3 也无 perl | `warn` 跳过，不误报 |
| 正常环境 | PASS |
| 故意塞一个真引用 | 抓出 FAIL 并进汇总 |

### 自检报了 FAIL，却不知道是哪个文件（我上一轮判断错了）

上一轮我看到 `FAIL ?attr/colorControlNormal 未声明`，判断是"注释里的假警报"。
**这个判断是错的。** 真实情况：独立仓里**还留着旧的 `drawable/ic_boss.xml`**，
它第 12 行有**真引用** `android:tint="?attr/colorControlNormal"`。

我当时只删了全量包里的那份，没删独立仓的。而两个仓库的 res 目录此后就**漂移**了。

> ⚠️ 旧文件的隐蔽性在于：Manifest 已经改成 `@mipmap/ic_launcher` 了，
> 看 Manifest 一切正常、自检报"icon 指向 @mipmap ✓"——
> 但旧文件不在任何引用链上，**照样会被 aapt 编译进 APK**，
> 于是资源链接照样失败，而且从引用关系上永远发现不了它。

**根因是我的检查只报属性名、不报文件路径。** 同一种报错可能是：
注释里的反面例子 / 真代码引用 / 没同步过去的旧文件——不给路径就只能猜。

改了三处：

1. **报出 `文件:行号`**（关键）
   ```
   FAIL  app/src/main/res/drawable/ic_boss.xml:8  用了 ?attr/colorControlNormal，但未声明
   ```
2. **新增 4.8 节**：专门查残留的旧图标文件 + Manifest 引用是否都存在。
3. **两边仓库 res 目录强制同步**，并加了 `diff` 校验步骤。

### 顺带修了一个会让 CI 放行的严重 bug

改上面那行时我写了 `echo "$hits" | while ...`，结果：

```
FAIL  app/src/main/res/drawable/ic_boss.xml:8 ...
结果：PASS（warn=0）        ← 明明有 FAIL 却报 PASS！
```

**管道把 `while` 放进了子 shell**，里面的 `fails=fails+1` 出了子 shell 就丢了。
后果极坏——日志里打了 FAIL，退出码却是 0，**CI 会把它放过去**。

改成 `done <<< "$hits"`（here-string），`while` 留在当前 shell，计数才准。
已实测：有真问题时退出码 1，干净时 0。

### android-37 只有次要版本，必须写 `compileSdkMinor`

CI 上终于装上了 android-37，但自检报：

```
ok    android-37 平台已装
ok    build-tools 36.0.0 已装
warn  android-37 只以次要版本形式存在（android-37.x）
```

**API 37 只以次要版本形式发行**（`android-37.0` / `android-37.1`），
**不存在**不带后缀的 `android-37`。而 AGP 找的是精确 hash：

| `compileSdkMinor` | AGP 去找 |
|---|---|
| 不写 | `android-37` → 必然失败 |
| `0` | `android-37.0` |
| `1` | `android-37.1` |

⚠️ 最坑的地方：**号码写错和平台没装，报的是同一句话**
`Failed to find target with hash string 'android-37'`。
所以"我明明装上了"和"号码不对"从报错上看不出区别——非常容易往错的方向查。

处理分两层：

1. **`app/build.gradle.kts` 里写 `compileSdkMinor = 0` 作为默认值**（带注释说明
   装的是 37.1 就必须改成 1，要求 AGP ≥ 9.1.0）。
2. **CI 里加一个对齐步骤**：读实际装上的 `android-37.x`，
   把文件里那个默认值 `sed` 成同一个号码，并**把改了什么打出来**。
   这样"C 机器装的是 37.1、仓库里写的是 0"这类漂移不会变成一次神秘失败。

### 顺手修的：失败项汇总

第一次只看到 `结果：FAIL=1`，**却看不到是哪一项 FAIL**——
日志太长，失败项散在中间被截断了。只剩个数字等于白跑。

现在脚本末尾会把失败项集中重打一遍：

```
结果：FAIL=1（warn=1）。

失败项：

    - compileSdk=36，期望 37
```

同时 SDK 段也从"笼统提示"改成**把实际装了哪些 37.x 列出来**：

```
->   已装的 37 次要版本：1
warn  compileSdkMinor=0，但装的是 android-37.1 —— 号码不一致
```

### 万一 latest 通道也拿不到 android-37

构建步骤会明确列出两条路并停掉，**不会**默默改成 36 假装能过：

- **A. 换更新的 runner 镜像**（改 apk job 的 `runs-on`）。
  官方对应：API 37 ↔ AGP 9.1.1 ↔ Gradle 9.3.1
  ↔ Android Studio Panda 3（2025.3.3 Patch 1）。
- **B. 走降级**（方案 2）：AGP 8.13.2 + Gradle 8.13 + compileSdk 36
  + Compose BOM 2026.06 + material3 1.4.0 —— 代价是丢掉整个 Expressive。

⚠️ 如果真走到 B，要改的不只是版本号：`ui/theme/Theme.kt` 得从
`MaterialExpressiveTheme` 退回普通 `MaterialTheme`，`ui/MainActivity.kt` 里的
`LoadingIndicator` 得换成 `LinearProgressIndicator`，`kotlinOptions` 那套
（AGP 9 已迁到顶层 `kotlin { compilerOptions {} }`）也要跟着回退。
**别只改版本号就以为完事了。**

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
