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

    private suspend fun run(vararg args: String): Veritpath.Result =
        withContext(singleThread) {
            lock.withLock {
                Veritpath.run(*args)
            }
        }

    /** 分析镜像：`analyze --brief --boot <path>` */
    suspend fun analyze(imagePath: String): Veritpath.Result =
        run("analyze", "--brief", "--boot", imagePath)

    /** 按指定 flag 分析（--init-boot / --boot / ...）。 */
    suspend fun analyzeWith(imagePath: String, flag: String): Veritpath.Result =
        run("analyze", "--brief", flag, imagePath)

    /** 分析并取 JSON：`analyze --json --boot <path>` */
    suspend fun analyzeJson(imagePath: String): String =
        withContext(singleThread) {
            lock.withLock { Veritpath.analyzeJson("--boot", imagePath) }
        }

    /**
     * 校验 payload 目录：`payload-check <dir>`
     * 这一步**不碰镜像**，适合在开修补之前先确认 payload 是完整的。
     */
    suspend fun payloadCheck(dir: String): Veritpath.Result =
        run("payload-check", dir)

    /**
     * 按给定分区角色分析一次，用来确认"这个镜像里到底有没有 ramdisk"。
     *
     * ⚠️ 关于 TARGET 的一个实测结论，很重要：
     *    TARGET 字段**只是回显你传进去的 role**，不是真正的分区识别——
     *      analyze --init-boot X  → TARGET:init_boot
     *      analyze --boot 同一份 X → TARGET:boot
     *    唯一有判断价值的是 **TARGET:none**（镜像里没有 ramdisk）。
     *    所以"自动识别 boot 还是 init_boot"在这套 CLI 上做不到，
     *    必须由用户指定——界面上就是那排分区选择。
     *
     * ⚠️ 另一个坑：**不要用上游的 `Veritpath.inject()`**。
     *    它拼出的是 `inject <image> -p <dir> -o <out>`——镜像是裸位置参数，
     *    而 CLI 的 load_images() 只认 --boot/--init-boot/--vendor-boot/--recovery，
     *    位置参数被 getopt 直接忽略。后果：没有镜像被加载，退出码 1。
     *    实测报错：`no input images given (use --boot/--init-boot/--vendor-boot)`
     */
    suspend fun probe(imagePath: String, role: String): Veritpath.Result =
        run("analyze", "--brief", roleFlag(role), imagePath)

    fun roleFlag(role: String): String = when (role) {
        "init_boot" -> "--init-boot"
        "boot" -> "--boot"
        "vendor_boot" -> "--vendor-boot"
        "recovery" -> "--recovery"
        else -> "--init-boot"
    }

    /**
     * 注入：`inject <--role> <image> -p <payloadDir> -o <outDir>`
     *
     * @param role 由 detectRole() 得到的 ramdisk 所在分区；为 null 时
     *             依次试 init_boot / boot（代价是两次解析，190MB 的镜像会慢一点）
     */
    suspend fun inject(
        imagePath: String,
        payloadDir: String,
        outDir: String,
        role: String? = null,
    ): Veritpath.Result = withContext(singleThread) {
        lock.withLock {
            if (role != null) {
                Veritpath.run("inject", roleFlag(role), imagePath,
                    "-p", payloadDir, "-o", outDir)
            } else {
                val a = Veritpath.run("inject", "--init-boot", imagePath,
                    "-p", payloadDir, "-o", outDir)
                if (a.ok()) a
                else Veritpath.run("inject", "--boot", imagePath,
                    "-p", payloadDir, "-o", outDir)
            }
        }
    }

    /** 刷之前自检：`verify <image>` */
    suspend fun verify(imagePath: String): Veritpath.Result =
        run("verify", imagePath)

    /** 镜像头诊断：解析失败时从这儿入手。 */
    suspend fun hexdump(imagePath: String): Veritpath.Result =
        run("hexdump", imagePath)

    /** 库版本，用来确认 .so 真的加载了（不是空壳）。 */
    suspend fun version(): String = withContext(singleThread) {
        lock.withLock { Veritpath.version() }
    }

    suspend fun clearCache() = withContext(Dispatchers.IO) {
        cacheDir().deleteRecursively()
    }
}

/** 把 Result 里的 KEY:VALUE 抽成几行给 UI 显示。 */
fun Veritpath.Result.fields(vararg keys: String): List<Pair<String, String>> =
    keys.mapNotNull { k -> line(k)?.let { k to it } }
