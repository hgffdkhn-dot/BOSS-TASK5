package com.boss.manager.data

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import dev.veritpath.Veritpath
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import java.io.File

/**
 * 镜像分析与修补（任务7：对接 veritpath 的 App 侧 JNI 库）。
 *
 * 这一层存在的意义，是替 UI 挡掉三个"编译能过、运行才炸"的坑。
 *
 * ## 坑 1：SAF 的 Uri 不是文件路径
 *
 * 用户通过系统文件选择器选到的是 `content://...`，**不是** `/sdcard/xxx`。
 * 直接把 `uri.toString()` 传给 veritpath，它会当成一个根本不存在的路径去 open，
 * 失败原因是 "No such file or directory"——而文件明明"选到了"。
 * 所以这里先把内容**拷进 app 私有缓存目录**，再拿真实路径去调用。
 *
 * ## 坑 2：`Veritpath.run()` 不是线程安全的
 *
 * 上游文档写得很清楚：内部用的是**进程级** stdout 重定向（dup2），
 * 并发调用会互相踩。所以这里用单线程调度器 + Mutex 双重串行化。
 * 单靠 Mutex 不够——Kotlin 协程的 Mutex 只保证同一时刻一个协程，
 * 但如果调度到不同线程、而输出捕获是进程级的，仍然可能出问题。
 *
 * ## 坑 3：别在主线程调
 *
 * GKI 1.0 的 boot.img 接近 190MB，解析要花时间。全部走 Dispatchers.IO。
 *
 * ## 权限：仍然为零
 *
 * SAF（系统文件选择器）由用户授权、Uri 自带临时读权限，
 * **不申请 READ/WRITE_EXTERNAL_STORAGE**。这是 BOSS 的硬要求，不能破。
 */
class PatchRepository(private val app: Context) {

    /** 进程级输出捕获决定了这一层必须串行。 */
    private val singleThread = Dispatchers.IO.limitedParallelism(1)
    private val lock = Mutex()

    /* ⚠️ setTempDir 为什么必须调：见 serial() 里的说明。 */

    @Volatile
    private var prepared = false

    /** 缓存目录：copied 的输入与产出的镜像都放这里。 */
    private fun cacheDir(): File =
        File(app.cacheDir, "veritpath").apply { mkdirs() }

    /**
     * 把 SAF 选中的 Uri 拷成私有缓存里的真实文件。
     * @return 可直接交给 veritpath 的绝对路径
     */
    suspend fun copyToCache(uri: Uri, name: String): String = withContext(Dispatchers.IO) {
        val dir = cacheDir()
        val out = File(dir, name)
        app.contentResolver.openInputStream(uri)?.use { input ->
            out.outputStream().use { output -> input.copyTo(output) }
        } ?: error("打不开选择的文件")
        if (!out.exists() || out.length() == 0L) error("拷出来的文件是空的")
        out.absolutePath
    }

    /** 从 Uri 猜一个文件名，兜底给个固定名——名字只是缓存用的，不影响结果。 */
    suspend fun guessName(uri: Uri, fallback: String): String = withContext(Dispatchers.IO) {
        runCatching {
            app.contentResolver.query(uri, null, null, null, null)?.use { c ->
                val i = c.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                if (i >= 0 && c.moveToFirst()) c.getString(i) else null
            }
        }.getOrNull() ?: fallback
    }

    /**
     * 把 SAF 选中的**目录**整个拷进缓存，返回缓存里的真实路径。
     *
     * 为什么必须拷：payload 是 `manifest.json + init.boss.rc + boss 二进制`
     * 的一个目录，而 SAF 给的是 `content://tree/...`。
     * veritpath 只认真实路径，所以目录也得落盘。
     *
     * 用 DocumentFile 递归遍历——`Uri` 本身没有"列目录"的能力。
     */
    suspend fun copyTreeToCache(treeUri: Uri): String = withContext(Dispatchers.IO) {
        val root = androidx.documentfile.provider.DocumentFile.fromTreeUri(app, treeUri)
            ?: error("打不开选择的目录")
        val dest = File(cacheDir(), "payload").apply {
            deleteRecursively()
            mkdirs()
        }
        copyTree(root, dest)
        dest.absolutePath
    }

    private fun copyTree(dir: androidx.documentfile.provider.DocumentFile, dest: File) {
        val files = dir.listFiles()
        if (files.isEmpty()) {
            // 空目录：payload-check 会因为缺 manifest.json 报出来，
            // 这里先把"一个文件都没拷到"说清楚，比让它报个含糊的错好。
            error("目录里没有文件（${dir.name}）——payload 至少要含 manifest.json")
        }
        for (f in files) {
            val out = File(dest, f.name ?: continue)
            if (f.isDirectory) {
                out.mkdirs()
                copyTree(f, out)
            } else {
                app.contentResolver.openInputStream(f.uri)?.use { input ->
                    out.outputStream().use { output -> input.copyTo(output) }
                }
            }
        }
    }

    /** 产出目录：inject 的 -o。 */
    fun outputDir(): String = File(cacheDir(), "out").apply { mkdirs() }.absolutePath

    /** 列出产出目录里的文件（修补后的镜像）。 */
    suspend fun outputs(): List<File> = withContext(Dispatchers.IO) {
        File(outputDir()).listFiles()?.toList()?.sortedBy { it.name } ?: emptyList()
    }

    // ---- 下面是真正调 JNI 的地方，全部串行 + 后台线程 ----

    /**
     * 所有 JNI 调用的统一入口：串行 + 后台线程 + 首次前设好临时目录（可选）。
     *
     * ## 为什么仍然串行
     * 上游用**进程级** stdout/stderr 重定向（dup2），并发调用会互相踩。
     * 双重保险：单线程调度器 + Mutex。只加 Mutex 不够——
     * 它只保证同一时刻一个协程，而 dup2 是进程级的。
     *
     * ## setTempDir：现在是**可选**的
     *
     * 上游第三次改（2026-09-29 同步）把捕获从文件换成了 **pipe 排空到内存**：
     * 不碰文件系统、不需要任何可写目录。Java 侧注释已改成
     * "You do not need this on Android or Linux"。
     *
     * 所以这里留着它只是**兜底**：只有在 `pipe()` 都创建失败的极端平台上，
     * native 才会退回用这个目录 mkstemp。Android/Linux 上永远不会走到。
     *
     * > 历史（别照着旧经验查问题）：
     * >  · 第一次：只捕获 stdout → 失败时输出为空
     * >  · 第二次：换成 mkstemp 找临时目录 → Android 上 /tmp、/data/local/tmp、
     * >    `.` 全都不可写 → 捕获静默失败，**输出同样为空**
     * >  · 第三次（现在）：pipe，不依赖路径
     * >
     * > 三次的现象完全一样（退出码有、输出无），根因各不相同。
     *
     * ## 为什么放在这里而不是构造函数
     * `setTempDir()` 内部会 `System.loadLibrary`。ViewModel 构造在主线程，
     * 而 loadLibrary 失败抛的是 **Error（UnsatisfiedLinkError）不是 Exception**
     * ——直接在主线程崩。挪到后台线程后由页面的 `catch (Throwable)` 接住，
     * 变成一条提示。
     */
    /**
     * ⚠️ block 必须是 `suspend () -> T`，不能写成 `() -> T`。
     *    写成非挂起 lambda 的话，里面调用的 `run(...)` 是 suspend，
     *    编译器会报 "Suspension functions can only be called within
     *    coroutine body"——而且报错指向**调用处**（每一处 serial { }），
     *    足足四行，看起来像四处都要改，实际上改这一处签名就行。
     */
    private suspend fun <T> serial(block: suspend () -> T): T =
        withContext(singleThread) {
            lock.withLock {
                if (!prepared) {
                    Veritpath.setTempDir(app.cacheDir.absolutePath)
                    prepared = true
                }
                block()
            }
        }

    /**
     * ⚠️ 输出有上限，超了会**静默截断**。
     *    native 侧把 pipe 调到 1MiB（best effort）并设成非阻塞，
     *    但直到 `vp_capture_stop()` 才排空——期间写入若超过缓冲区就返回 EAGAIN，
     *    那部分内容直接丢了，不报错、不提示。
     *
     *    正常命令（plan / analyze --brief）几 KB 远够。
     *    但 `--json` 或 `-v` 的冗长输出有风险。所以 UI 别默认开 verbose。
     */
    private suspend fun run(vararg args: String): Veritpath.Result =
        serial { Veritpath.run(*args) }

    /** 分析镜像：`analyze --brief --boot <path>` */
    suspend fun analyze(imagePath: String): Veritpath.Result =
        run("analyze", "--brief", "--boot", imagePath)

    /** 按指定 flag 分析（--init-boot / --boot / ...）。 */
    suspend fun analyzeWith(imagePath: String, flag: String): Veritpath.Result =
        run("analyze", "--brief", flag, imagePath)

    /** 分析并取 JSON。上游新签名收 Image 而不是裸字符串。 */
    suspend fun analyzeJson(imagePath: String, role: String? = null): String =
        serial {
            val flag = if (role == null || role == "auto") null else roleFlag(role)
            run(*buildList {
                add("analyze"); add("--json")
                flag?.let { add(it) }
                add(imagePath)
            }.toTypedArray()).output
        }

    /**
     * 校验 payload 目录：`payload-check <dir>`
     * 这一步**不碰镜像**，适合在开修补之前先确认 payload 是完整的。
     */
    suspend fun payloadCheck(dir: String): Veritpath.Result =
        run("payload-check", dir)

    /** 分区角色 → CLI flag。 */
    fun roleFlag(role: String): String = when (role) {
        "init_boot" -> "--init-boot"
        "boot" -> "--boot"
        "vendor_boot" -> "--vendor-boot"
        "recovery" -> "--recovery"
        else -> "--init-boot"
    }

    /* ⚠️ 上游 0.2.0 已修掉"位置参数被忽略"这个 bug（main.c 新增 guess_role：
     *    按 magic 区分 vendor_boot / boot，再用"无 kernel 但有 ramdisk"
     *    判定 init_boot）。所以现在**可以用 Image.auto() 自动识别**了。
     *    之前必须让用户手选分区，就是因为 TARGET 字段只是回显传入的 role。
     *
     *    实测（上游新 CLI，裸位置参数）：
     *      init_boot.img    → rc=0，产出 init_boot.veritpath.img ✓
     *      legacy1/legacy2  → rc=0 ✓
     *      vendor_boot.img  → rc=0 ✓
     *      boot.img         → rc=1 "nothing was patched"（它只有 kernel 无 ramdisk，
     *                         这是**正确**行为，不是 bug）
     */

    /**
     * 分析一次，用来确认"这个镜像里到底有没有 ramdisk"。
     *
     * @param role null/auto 时让 CLI 按内容识别
     */
    /* ================================================================
     * ⚠️ 上游 Java 绑定的拼装 bug（**已实测确认**，不要再用 Veritpath.analyze/inject）
     *
     * 上游 Java 绑定里（Veritpath 那个类）：
     *     String[] head = {"inject", "-p", payload, "-o", out};
     *     return run(concat(withFlags(image), head, extraArgs));
     *
     * concat(first=withFlags, second=head) 把镜像 flag 排在了子命令**前面**，
     * 拼出来是：
     *     ["--init-boot", "/path", "inject", "-p", pay, "-o", out]
     *
     * 而 native 的 vp_cli_run() 取 **argv[0]** 当子命令，于是：
     *     JNI 侧  → "veritpath: unknown command: --init-boot"，退出码 1
     *     CLI 侧  → GNU getopt 会重排参数，实际跑成了 unpack（更隐蔽！）
     *
     * 实测（同一个 harness）：
     *     [上游拼法] rc=1  → unknown command: --init-boot
     *     [正确拼法] rc=0  → 正常产出镜像
     *
     * 所以上游那三个辅助方法**全都不能用**。
     * 上游的 tools/test_jni.sh 测不到这个 bug——它用自己的 harness
     * 直接传 ["analyze","--brief","--boot",img]，绕过了 Java 的拼装。
     *
     * 本文件所有调用都改成自拼 argv：**子命令固定放 argv[0]**。
     * ================================================================ */

    suspend fun probe(imagePath: String, role: String? = null): Veritpath.Result =
        // 同样自己拼：上游那几个辅助方法的拼装顺序是错的（见下）。
        serial {
            val flag = if (role == null || role == "auto") null else roleFlag(role)
            run(*buildList {
                add("analyze"); add("--brief")
                flag?.let { add(it) }
                add(imagePath)
            }.toTypedArray())
        }

    /**
     * 注入。默认用 `Image.auto()` 让 CLI 自己识别分区类型。
     *
     * ⚠️ **不要用裸字符串当镜像路径**——上游新 API 已经把这点做成类型安全了
     *    （`Veritpath.Image` 必须由工厂方法构造，flag 不会漏）。
     */
    suspend fun inject(
        imagePath: String,
        payloadDir: String,
        outDir: String,
        role: String? = null,
    ): Veritpath.Result = serial {
        run(*injectArgs(imagePath, payloadDir, outDir, role, false))
    }

    /**
     * 拼 inject 的 argv。**子命令必须在 argv[0]**。
     *
     * ⚠️ 为什么不用上游那个 inject 辅助方法：见下面
     *    [上游 Java 绑定的拼装 bug]，它拼出来的顺序根本执行不了。
     */
    private fun injectArgs(
        imagePath: String,
        payloadDir: String,
        outDir: String,
        role: String?,
        keepTrailing: Boolean,
    ): Array<String> {
        val flag = if (role == null || role == "auto") null else roleFlag(role)
        return buildList {
            add("inject")
            flag?.let { add(it) }
            add(imagePath)
            add("-p"); add(payloadDir)
            add("-o"); add(outDir)
            if (keepTrailing) add("--keep-trailing")
        }.toTypedArray()
    }

    /**
     * 注入，**保留尾部填充**（`--keep-trailing`）。
     *
     * ## 现象
     * 从手机 `dd` 出来的镜像是整个分区（常见 100MB+），而真正的内容只有几十 MB，
     * 剩下的都是分区尾部的填充。repack 时这些东西会被丢掉——
     * 于是"一百多 MB 进去、42MB 出来"。
     *
     * 实测（8KB 镜像 + 3MB 填充）：
     *   不加 --keep-trailing → 8192 字节
     *   加   --keep-trailing → 3008192 字节（原样保留）
     *
     * ## 要不要保留
     * **默认保留。** 丢掉填充功能上通常没事（刷进去照样能开机，
     * 因为 fastboot 只写镜像声明的长度），但保真更稳：
     *   · 尾部万一不是纯零（OEM 数据、另一个槽的残留），丢了不可逆
     *   · 用户看到体积骤降会以为修补坏了，本身就是个信任问题
     * 代价只是文件大一点。
     *
     * ⚠️ 上游 Java 侧没给这个选项做专门 API（`Veritpath` 里没有 keepTrailing），
     *    但 `inject(Image, dir, out, extraArgs...)` 收可变参数，可以传 flag。
     */
    suspend fun injectKeepTrailing(
        imagePath: String,
        payloadDir: String,
        outDir: String,
        role: String? = null,
    ): Veritpath.Result = serial {
        run(*injectArgs(imagePath, payloadDir, outDir, role, true))
    }

    /** 刷之前自检：`verify <image>` */
    suspend fun verify(imagePath: String): Veritpath.Result =
        run("verify", imagePath)

    /** 镜像头诊断：解析失败时从这儿入手。 */
    suspend fun hexdump(imagePath: String): Veritpath.Result =
        run("hexdump", imagePath)

    /** 库版本，用来确认 .so 真的加载了（不是空壳）。 */
    suspend fun version(): String = serial { Veritpath.version() }

    suspend fun clearCache() = withContext(Dispatchers.IO) {
        cacheDir().deleteRecursively()
    }
}

/** 把 Result 里的 KEY:VALUE 抽成几行给 UI 显示。 */
fun Veritpath.Result.fields(vararg keys: String): List<Pair<String, String>> =
    keys.mapNotNull { k -> line(k)?.let { k to it } }
