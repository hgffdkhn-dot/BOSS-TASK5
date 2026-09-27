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

## 5. 三条别做错的事

- **别把 APK 塞进 payload。** 写系统分区违反无修改原则，这是 BOSS 的定位底线。
- **别把 keystore 提交进仓库。** 要签正式包就在 CI 里用 secrets。
- **别为了 CI 变绿放宽 Gradle 校验。** 放宽了就等于把"版本不匹配"藏起来，
  它会在真机上以更难查的形式冒出来。
