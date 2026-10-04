package com.boss.manager.core

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File

/**
 * 取 root 的通道：**优先 BOSS 自己的，退回设备上的 su**。
 *
 * 为什么必须有"退回"这一路：
 *   本机安装这个功能的用处，恰恰是 **BOSS 还没装的时候**——
 *   那时 /data/adb/boss/boss 不存在，BossIpc 连不上 daemon，
 *   唯一能用的就是设备上已有的 su（Magisk / KernelSU / 临时 root）。
 *   少一路，这个功能就在它最该有用的场景下失效。
 *
 * ⚠️ 直接 exec 系统 su 这条路**没有上真机验证过**：
 *   App 是 untrusted_app，能否 fork/exec 某个 su 取决于那个 su 的
 *   路径权限与 SELinux 策略（各 root 方案差别很大）。
 *   所以探测是逐个试，而不是假定某一个一定在。
 */
object RootShell {

    data class Result(val exitCode: Int, val output: String) {
        val ok: Boolean get() = exitCode == 0
    }

    /** 已知的 su 位置，按常见程度排序。逐个试，第一个能用的胜出。 */
    private val SU_CANDIDATES = listOf(
        "/data/adb/ksu/bin/su",   // KernelSU
        "/data/adb/magisk/su",    // Magisk（新布局）
        "/system/bin/su",
        "/sbin/su",
        "/su/bin/su",
        "/debug_ramdisk/su",
    )

    /** 缓存探测结果：一次开机内分区布局不会变，但 su 方案可能被切换。 */
    @Volatile
    private var cachedSu: String? = null

    sealed interface Source {
        /** BOSS 自己的 daemon（已安装时的首选） */
        data object Boss : Source
        /** 设备上的第三方 su */
        data class SystemSu(val path: String) : Source
    }

    /**
     * 探测可用的 root 来源。
     * @return null 表示拿不到 root
     */
    suspend fun detect(): Source? = withContext(Dispatchers.IO) {
        // 1) BOSS 已装：这是首选，因为它有授权弹窗与审计，
        //    不会静默把 root 交出去。
        if (BossIpc.ping()) {
            val r = BossIpc.exec(
                "/system/bin/id -u",
                flags = BossIpc.Flag.NOLOG,
                timeoutMs = 8_000,
            )
            if (r.output.trim() == "0") return@withContext Source.Boss
        }

        // 2) 设备上的 su
        cachedSu?.let { return@withContext Source.SystemSu(it) }
        for (su in SU_CANDIDATES) {
            if (!File(su).canExecute()) continue
            val r = runCatching { rawExec(listOf(su, "-c", "id -u"), 8_000) }
                .getOrNull() ?: continue
            if (r.exitCode == 0 && r.output.trim() == "0") {
                cachedSu = su
                return@withContext Source.SystemSu(su)
            }
        }
        null
    }

    /** 执行一条 root 命令。走探测到的通道。 */
    suspend fun exec(cmd: String, timeoutMs: Long = 60_000): Result =
        withContext(Dispatchers.IO) {
            val src = detect()
                ?: return@withContext Result(127, "拿不到 root：设备上没有可用的 su，BOSS 也还没装")
            when (src) {
                is Source.Boss -> {
                    val r = BossIpc.exec(cmd, flags = 0, timeoutMs = timeoutMs.toInt())
                    Result(r.exitCode, r.output)
                }
                is Source.SystemSu -> rawExec(listOf(src.path, "-c", cmd), timeoutMs)
            }
        }

    // ------------------------------------------------------- 文件读取辅助
    //
    // 给 RamdiskProbe 用：探测 ramdisk 需要读文件头，而 App 对大多数
    // 候选路径（/data/local/tmp 等）没有读权限——必须先拿到 root 通道。

    /** 同步版 exec。探测逻辑在协程里跑，这里不需要再切一次。 */
    private fun rawExecSync(argv: List<String>, timeoutMs: Long): Result {
        return runCatching {
            val p = ProcessBuilder(argv).redirectErrorStream(true).start()
            val out = StringBuilder()
            val pump = Thread {
                runCatching {
                    p.inputStream.bufferedReader().forEachLine { out.appendLine(it) }
                }
            }
            pump.start()
            if (!p.waitFor(timeoutMs, java.util.concurrent.TimeUnit.MILLISECONDS)) {
                p.destroyForcibly()
                return Result(124, "")
            }
            pump.join(2_000)
            Result(p.exitValue(), out.toString())
        }.getOrElse { Result(126, "") }
    }

    /**
     * 执行一条 shell 并取 stdout，自动挑通道。
     *
     * ⚠️ 不能只认第三方 su：BOSS 已装的机器上 `cachedSu` 是 null
     *    （那条路从没走过），但 daemon 通道完全可用。
     *    只认 su 会让"装了 BOSS 的机器反而读不到文件头"。
     */
    private fun shSync(cmd: String, timeoutMs: Long = 10_000): String? {
        val su = cachedSu
        if (su != null) {
            val r = rawExecSync(listOf(su, "-c", cmd), timeoutMs)
            return if (r.ok) r.output else null
        }
        if (runCatching { BossIpc.ping() }.getOrDefault(false)) {
            val r = runCatching {
                BossIpc.exec(cmd, flags = BossIpc.Flag.NOLOG, timeoutMs = timeoutMs.toInt())
            }.getOrNull()
            return r?.takeIf { it.ok() }?.output
        }
        return null
    }

    /** 用 root 读文件前 n 字节，返回 hex（无分隔）。 */
    fun headHexSync(path: String, n: Int): String? {
        val out = shSync("dd if='$path' bs=1 count=$n 2>/dev/null | od -An -tx1")
            ?: return null
        return out.filter { it.isDigit() || it in 'a'..'f' || it in 'A'..'F' }
            .takeIf { it.isNotEmpty() }
    }

    /** 用 root 取文件大小。 */
    fun sizeOfSync(path: String): Long? =
        shSync("stat -c %s '$path'")?.trim()?.toLongOrNull()

    /** 用 root 打开一个文件的输入流（给 gzip 解压确认用）。 */
    fun openRead(path: String): java.io.InputStream? {
        val su = cachedSu ?: return null   // 只有第三方 su 能直接给流；
        return runCatching {                // BOSS 通道走 shSync 拿不到流
            ProcessBuilder(listOf(su, "-c", "cat '$path'")).start().inputStream
        }.getOrNull()
    }

    /** 直接 fork/exec，不走 daemon。仅用于第三方 su。 */
    private fun rawExec(argv: List<String>, timeoutMs: Long): Result {
        return runCatching {
            val p = ProcessBuilder(argv)
                .redirectErrorStream(true)
                .start()
            val out = StringBuilder()
            val reader = p.inputStream.bufferedReader()
            // 边读边等：先 waitFor 再读的话，输出多时子进程会阻塞在写管道上，
            // 表现为命令"卡住不返回"——dd 一个 100MB 分区时很容易撞到。
            val pump = Thread {
                runCatching {
                    var line: String?
                    while (reader.readLine().also { line = it } != null) out.appendLine(line)
                }
            }
            pump.start()
            val done = p.waitFor(timeoutMs, java.util.concurrent.TimeUnit.MILLISECONDS)
            if (!done) {
                p.destroyForcibly()
                return Result(124, "命令超时（${timeoutMs}ms）")
            }
            pump.join(2_000)
            Result(p.exitValue(), out.toString())
        }.getOrElse { Result(126, "执行失败：${it.message}") }
    }
}
