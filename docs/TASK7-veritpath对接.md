# BOSS · 任务7：veritpath App 侧对接

> 上游 `hgffdkhn-dot/veritpath` 新增了 App 对接（`docs/ANDROID_APP.md`）。
> 本文记录 BOSS 客户端怎么把它接进来、为什么这么接、以及哪些坑已经踩过。

## 0. 一句话结论

按上游文档的**方案 A：JNI 动态库**落地，vendored 源码进
`app/src/main/cpp/veritpath/`，App 新增一个「修补」页。
**不做刷入**——刷镜像必须走 fastboot。

---

## 1. 为什么选方案 A 而不是方案 B

| | 方案 A（JNI .so） | 方案 B（打包二进制 + exec） |
|---|---|---|
| 需要的权限 | 无 | 需要可执行目录 |
| 稳定性 | 高 | 受 `noexec` 挂载、SELinux 影响 |
| 每 ABI 一份 | Gradle 自动拆分 | 自己判断 `Build.SUPPORTED_ABIS` |

决定性的一条：**方案 B 在 sdcard 上永远执行不了**（`noexec` 挂载），
而 Android 10+ 对应用私有目录执行二进制的限制越来越严。

顺带说一句：方案 B 的这个坑我们在 CI 上已经吃过一次同款——
`build/` 目录被 `noexec` 挂载时，探针编译成功但 exec 报
`Permission denied`。不是同一处，但是同一类问题。

---

## 2. 落了什么

```
app/src/main/cpp/veritpath/
├── CMakeLists.txt          新增（含 VP_NO_MAIN 与 visibility 说明）
├── veritpath_jni.c         上游原样
└── src/*.c + vp.h          上游 vendored（9 个 .c）

app/src/main/cpp/CMakeLists.txt        add_subdirectory(veritpath)
app/src/main/java/dev/veritpath/Veritpath.java    上游原样
app/src/main/java/com/boss/manager/
├── data/PatchRepository.kt   SAF→路径、串行化、JNI 包装
├── ui/PatchViewModel.kt      页面状态（AndroidViewModel，要 Context）
└── ui/screens/PatchScreen.kt 「修补」页
```

**只 `add_subdirectory`，不 `target_link_libraries` 到 bossipc。**
两个 .so 完全独立：`libbossipc.so` 管运行时、`libveritpath.so` 管装机期。
AGP 会把 CMake 产出的所有 .so 打进 APK，所以不链接照样打包。

---

## 3. 三个"编译能过、运行才炸"的坑

### ① `Veritpath.java` 的包名不能动

C 侧函数名是 `Java_dev_veritpath_Veritpath_nativeRun`。
把 Java 类整理到 `com.boss.manager` 下是很自然的动作，但 C 侧不跟着改的话：
**编译、链接、打包三步全绿**，只在 `System.loadLibrary` 之后抛
`java.lang.UnsatisfiedLinkError: No implementation found`。

自检脚本 4.9 节会比对这两边。

### ② SAF 的 Uri 不是文件路径

用户通过系统文件选择器选到的是 `content://...`。
直接把它传给 veritpath，它会当路径去 open，报 `No such file or directory`——
而文件明明"选到了"。

`PatchRepository` 先把内容**拷进 app 私有缓存目录**，再拿真实路径去调用。
payload 是目录，用 `DocumentFile` 递归拷（因此新增了 `androidx.documentfile` 依赖）。

### ③ `Veritpath.run()` 不是线程安全的

上游文档写得很清楚：内部用**进程级** stdout 重定向（dup2），并发调用会互相踩。

所以 `PatchRepository` 用了**双重串行化**：
`Dispatchers.IO.limitedParallelism(1)` + `Mutex`。
只加 Mutex 不够——它保证同一时刻一个协程，但进程级的输出捕获不认协程。

另外：GKI 1.0 的 boot.img 接近 190MB，**务必在后台线程调**。

---

## 3.5 「修补失败，退出码 1，输出无」——两个 bug，上游修了一个

### ③ 位置参数被忽略 → **上游 0.2.0 已修**

旧的 `Veritpath.inject()` 拼出 `inject <image> -p <dir> -o <out>`，
镜像是裸位置参数，而 CLI 只认 `--boot/--init-boot/--vendor-boot/--recovery`，
于是路径被 `getopt` 直接丢掉 → `no input images given`。

上游的修法（两处，都已同步过来）：

- **`main.c` 新增 `guess_role()`**：位置参数不再是"被忽略"，而是先读文件、
  按 magic 区分 `vendor_boot` / `boot`，再用"无 kernel 但有 ramdisk"
  判定 `init_boot`。
- **`Veritpath.java` 新增 `Image` 类**：`Image.boot/initBoot/vendorBoot/recovery/auto(path)`，
  由工厂方法构造，**flag 不可能再漏**。`auto()` 对应裸路径，交给 CLI 识别。

实测（上游新 CLI，裸位置参数）：

| 镜像 | 结果 |
|---|---|
| `init_boot.img` | rc=0 → `init_boot.veritpath.img` ✓ |
| `legacy1/legacy2.img` | rc=0 ✓ |
| `vendor_boot.img` | rc=0 ✓ |
| `boot.img` | rc=1 `nothing was patched` —— **正确行为**，它只有 kernel 无 ramdisk |

**BOSS 侧跟着改**：`PatchRepository` 改用新的 `Image` API，默认 `Image.auto()`；
界面分区选择默认改成 **auto**，手选项降级为兜底。

> 实现细节：`Image.flag` 是**包私有**的（只在 `dev.veritpath` 内可见）。
> 我们在 `com.boss.manager.data` 里访问 `img.flag` 会直接编译不过——
> 所以 `probe()` 用 `Veritpath.analyze(image)` 而不是手工拼 argv。

### ④ 输出捕获：上游改了**三次**，现在的形态是 pipe

这是整个对接里最值得记的一段——**同一个现象（退出码有、输出无），
上游前后改了三次，根因各不相同**：

| 轮次 | 上游实现 | 在 Android 上的表现 |
|---|---|---|
| 1 | `dup2` 只覆盖 **stdout** | 错误走 stderr → 输出为空 |
| 2 | 改用 `mkstemp()` 找临时目录 | `/tmp` 不存在、`/data/local/tmp` 属 shell、`.` 是 `/` → **捕获根本没启动**，输出同样为空 |
| 3（现在） | **pipe 排空到内存** | 不碰文件系统、不需要任何可写目录 ✓ |

第三次的 `util.c` 头部注释把原因写得很清楚：

> It must not touch the filesystem. An Android app has no writable /tmp,
> the current directory is "/" (not writable), TMPDIR is unset, and
> /data/local/tmp belongs to the shell ... Guessing at those paths just
> reintroduces the failure.

**所以：**

- **BOSS-PATCH 必须删掉**（第二轮加的 stderr 补丁）——留着会和上游的 dup2
  抢同一个 fd。自检 4.9 节是反向检查：发现 `BOSS-PATCH` 就报 FAIL。
- **`setTempDir` 现在是可选的**。Java 侧注释已改成
  "You do not need this on Android or Linux"。我们仍保留调用作为兜底
  （只在 `pipe()` 都创建失败的极端平台上才用得到），无害。

实测（同步后源码，**不调 setTempDir**、cwd 为 `/`、`TMPDIR` 未设）：

```
[auto 无tempdir] rc=0 | ==> backup of ... -> wrote ...
[坏payload     ] rc=1 | veritpath: cannot load payload from /nope
[version       ] rc=0 | veritpath 0.2.0
```

⚠️ **一个残留限制：输出超过上限会静默截断。**
native 把 pipe 调到 1MiB（best effort，`F_SETPIPE_SZ`）并设成非阻塞，
但要到 `vp_capture_stop()` 才排空——期间写入超限就返回 EAGAIN，
那部分内容直接丢，**不报错也不提示**。
正常命令（plan / `analyze --brief`）几 KB 远够，
但 `--json` 或 `-v` 的冗长输出有风险。**UI 不要默认开 verbose。**

### 附：TARGET 字段不能用来自动识别分区（旧结论，已过时）

### 附：TARGET 字段不能用来自动识别分区（旧结论，已过时）

### 附：TARGET 字段不能用来自动识别分区（旧结论，现已部分过时）

上游修好之前，实测：

```
analyze --brief --init-boot X  → TARGET:init_boot
analyze --brief --boot      X  → TARGET:boot
```

TARGET 只是回显传入的 role。现在有了 `guess_role()` 这条结论**不再适用**
——自动识别已经可用。保留这段只是说明"当初为什么必须让用户手选"。

## 3.6 大镜像修补后变小：填充（trailing）与 `--keep-trailing`

### 现象

从手机 `dd` 出来的镜像是**整个分区**（常见 100MB+），而真实内容只有几十 MB，
剩下的是分区尾部填充。repack 时这部分会被丢掉，于是"一百多 MB 进去、42MB 出来"。

**这不是修补坏了，刷进去照样能开机**（fastboot 只写镜像声明的长度）。

### 实测

```
$ veritpath inject big.img -p payloads/example-su -o out/
==> big.img: 2.9MiB of padding after the image was dropped (whole-partition dump);
    use --keep-trailing to carry it over
源 3008192 → 产    8192

$ veritpath inject big.img -p payloads/example-su -o out/ --keep-trailing
源 3008192 → 产 3008192   ← 原样保留
```

上游在丢填充时会主动打一条日志说明原因——这个设计很好。

### 上游功能 vs Java 侧 API

| | 状态 |
|---|---|
| CLI `--keep-trailing` | ✅ 有（`main.c`，短选项 `-K`） |
| Java 侧专门 API | ❌ **没有** |

所以靠 `inject(...)` 的可变参数传 flag。**但下面这个 bug 让那条路走不通。**

## 3.7 上游 Java 绑定的拼装顺序 → **已修（含守卫）**

上一轮发现的 bug（`concat(withFlags(image), head, ...)` 把镜像 flag 排到了
子命令前面，导致 `unknown command`）**上游已修**：

- `analyze` / `analyzeJson` / `inject` 四处都改成 `concat(head, withFlags(...), ...)`，
  子命令回到 argv[0]。
- `run()` 加了显式守卫：`args[0]` 以 `-` 开头就抛
  `IllegalArgumentException("the sub-command must be args[0] ...")`。
- `veritpath_jni.c` 里也加了同样的检查，并给出可读提示。

实测（同步后源码）：

```
[旧顺序(flag在前)   ] rc=1 | veritpath: argv[0] must be the sub-command, got '--init-boot'
[新顺序(head在前)   ] rc=0 | 正常产出
[自拼+keep-trailing ] rc=0 | 正常产出
```

**BOSS 侧仍保持自拼 argv**，不改用上游辅助方法——自拼已经工作，
而且少一层间接。自检 4.9 节改为检查"上游守卫还在不在"，
它决定了这类错误是说清楚的报错还是莫名其妙的 `unknown command`。

上游 `tools/test_jni.sh` 也新增了 `argv: flag-first is diagnosed` 一项。

## 3.7.1 上游 Java 绑定的拼装顺序（**历史问题，已修**）

上游 Java 里：

```java
String[] head = {"inject", "-p", payload, "-o", out};
return run(concat(withFlags(image), head, extraArgs));
```

`concat(first=withFlags, second=head)` 把**镜像 flag 排在了子命令前面**：

```
["--init-boot", "/path", "inject", "-p", pay, "-o", out]
                ↑ argv[0] 变成了 flag
```

而 native 的 `vp_cli_run()` 取 **argv[0]** 当子命令。实测：

| 调用方式 | 结果 |
|---|---|
| 上游拼法 | JNI：`unknown command: --init-boot`，rc=1 |
| 上游拼法 | CLI：GNU getopt 会重排参数，**实际跑成了 unpack**（更隐蔽） |
| 自拼（子命令在 argv[0]） | rc=0，正常产出 |

**结论：上游的 `analyze()` / `inject()` / `analyzeJson()` 三个辅助方法全都不能用。**
BOSS 侧已全部改为自拼 argv，子命令固定放首位。

> 上游的 `tools/test_jni.sh` 测不到这个 bug——它用自己的 harness 直接传
> `["analyze","--brief","--boot",img]`，正好绕过了 Java 的拼装逻辑。
> 所以"上游自检 PASS"不代表 Java 绑定能用。

自检脚本 4.9 节加了守卫生意代码不许再调那三个方法。

## 4. 仍然不做的事：刷入## 4. 仍然不做的事：刷入

「修补」页的终点是**导出修补后的镜像**，之后由用户
`fastboot flash init_boot`。App 在 Android 上没有访问 bootloader 的能力，也不该有。

这个边界在界面上写明了。**不要**让人以为点了就能装好——
那种误解比没有这个功能更糟。

权限：**仍然为零**。SAF 由用户授权、Uri 自带临时读权限，
不申请 `READ/WRITE_EXTERNAL_STORAGE`。这是 BOSS 的硬要求。

---

## 5. 验证到哪一步（诚实版）

| 项 | 状态 |
|---|---|
| 上游 `tools/test_jni.sh` | ✅ PASS（version / analyze / 错误处理） |
| **用 vendored 那份源码**跑同一套 | ✅ PASS（确认复制没漏文件） |
| C 层 host 语法检查 | ✅ 9 个 .c 全过 |
| JNI 符号族一致性 | ✅ 静态比对通过 |
| 检出缺失 import | ✅ 无（72 条"缺失"全是成员调用的误报） |
| **Gradle 实际编出 .so** | ❌ **未验**（沙盒无 Android SDK/NDK） |
| 大镜像 trailing 保留 | ✅ CLI 实测（3008192 → 3008192） |
| Java 拼装顺序 bug | ✅ JNI harness 实测（rc=1 vs rc=0） |
| **真机 `System.loadLibrary`** | ❌ 未验 |
| **真机分析一个真实镜像** | ❌ 未验 |

vendored 那份是拿上游的 `tools/test_jni.sh` 完整跑过的（version、`analyze --brief`
含 `ARCH:`、错误命令返回非零），所以 JNI 层本身有问题这件事基本可以排除。
但 **NDK 编译和真机加载仍未验**。

---

## 6. 上游同步

vendored 的代价就是会漂移。同步方法：

```bash
curl -sL https://codeload.github.com/hgffdkhn-dot/veritpath/tar.gz/refs/heads/main -o /tmp/vp.tgz
tar xzf /tmp/vp.tgz --strip-components=1 -C /tmp/vp
cp /tmp/vp/src/*.c /tmp/vp/src/vp.h app/src/main/cpp/veritpath/src/
cp /tmp/vp/jni/veritpath_jni.c app/src/main/cpp/veritpath/
cp /tmp/vp/jni/dev/veritpath/Veritpath.java app/src/main/java/dev/veritpath/
```

> ⚠️ 沙盒里 `git clone` 是 403，但 `codeload` 的 tar.gz 能取到（200）。
> 需要在脚本里拉上游时用后者。

同步后**必须做两件事**：

1. **确认 `setTempDir` 还接在**（3.5 节 ⑤）——上游换 `mkstemp()` 之后，
   不调它就输出恒为空。自检 4.9 节会检查。
2. **本地补丁的存废**要看上游走到哪一步了：
   现在捕获已由上游用 pipe 实现，**BOSS-PATCH 必须删**（留着会和上游 dup2 抢 fd），
   `setTempDir` 降级为可选兜底。自检 4.9 节是反向检查——
   发现 `BOSS-PATCH` 就报 FAIL。
3. 跑一次 `bash tools/check_android_toolchain.sh`，4.9 节会打出上游
   `VP_VERSION` 与 `.c` 计数，数字变了就说明上游动过。

> 上游的 `VP_VERSION` 目前仍是 `0.2.0`，**版本号不变不代表内容没变**——
> 这次的修复就是在同一个版本号下发生的。所以别只看版本号，
> 要用 `diff` 比对 `src/` 与 `jni/`。

---

## 7. 首次真编译可能要调的

- **NDK 版本**：veritpath 用 C11 + zlib，主流 NDK 都行，但
  `ANDROID_PLATFORM` 至少要 android-24（我们 minSdk 26，天然满足）。
- **`Icons.Default.Build`**：修补页 tab 用它。已在 `material-icons-extended` 里。
- 若报 `undefined reference: vp_xxx`，多半是 vendored src 漏拷了文件——
  先看自检脚本 4.9 节的 `.c` 计数。
