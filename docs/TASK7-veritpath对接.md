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

## 4. 仍然不做的事：刷入

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

同步后跑 `bash tools/check_android_toolchain.sh`，4.9 节会打出上游
`VP_VERSION`，数字变了就说明上游动过。

---

## 7. 首次真编译可能要调的

- **NDK 版本**：veritpath 用 C11 + zlib，主流 NDK 都行，但
  `ANDROID_PLATFORM` 至少要 android-24（我们 minSdk 26，天然满足）。
- **`Icons.Default.Build`**：修补页 tab 用它。已在 `material-icons-extended` 里。
- 若报 `undefined reference: vp_xxx`，多半是 vendored src 漏拷了文件——
  先看自检脚本 4.9 节的 `.c` 计数。
