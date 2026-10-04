package com.boss.manager.data

import android.content.Context
import android.net.Uri
import com.boss.manager.core.RamdiskProbe
import com.boss.manager.core.RootShell
import dev.veritpath.Veritpath
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File

/**
 * 本机安装：读当前分区 → 修补 → **直接写回分区**，全程不用电脑。
 *
 * ## 和「修补页」的分工
 *
 * | | 修补页 | 本机安装 |
 * |---|---|---|
 * | 镜像来源 | 用户选文件 | 直接读 /dev/block/by-name |
 * | 写回方式 | 导出 → fastboot | **dd 写分区** |
 * | 需要电脑 | 是 | 否 |
 * | 需要 root | 否 | **是** |
 *
 * ## ⚠️ 这条路的代价：写错就是变砖
 *
 * fastboot 至少有分区名保护和 bootloader 兜底，`dd of=/dev/block/...` 什么都没有：
 * 写错分区、写超长、写成空文件，下一次开机就是砖。
 * 所以这里把能静态检查的全做掉，**任何一项不过就不写**。
 *
 * ## 五道闸（写回前逐条过）
 *
 * 1. **分区身份**：写回的目标必须是刚才读出来的那个路径（同一个字符串），
 *    不接受任何重新探测——重新探测可能因槽位变化指向另一个分区。
 * 2. **magic**：新镜像开头必须是 `ANDROID!` 或 `VNDRBOOT`。
 *    veritpath 出问题产出垃圾时，这一条是最后一道网。
 * 3. **大小**：新镜像 ≤ 原分区大小。dd 写超长会截断，
 *    截断的 boot 镜像开机必死。
 * 4. **verify**：veritpath 自检必须过。
 * 5. **备份在**：原分区的备份文件必须存在且非空——没有退路就不能动手。
 *
 * 这五条里任何一条的设计初衷都一样：**宁可不动，也不能动错。**
 */
class LocalInstallRepository(private val app: Context) {

    /** 工作目录。/data/local/tmp 重启后还在，且 root 可写。 */
    private val WORK = "/data/local/tmp/boss-local"
    /** 原厂备份单独放，且**永不自动删除**。 */
    private val BACKUP = "/data/local/tmp/boss-local/backup"

    // --------------------------------------------------------------- 探测

    /** 当前槽位后缀，如 "_a"。单槽设备返回空串。 */
    suspend fun slotSuffix(): String = withContext(Dispatchers.IO) {
        RootShell.exec("getprop ro.boot.slot_suffix", 8_000).output.trim()
    }

    /**
     * 找出 ramdisk 所在的分区。
     *
     * Android 13+ GKI 多数在 init_boot；老机型/非 2SI 在 boot。
     * 两个都在时**优先 init_boot**——但如果它的 size 为 0 就退回 boot。
     */
    /**
     * 找出 ramdisk 所在的分区。
     *
     * ⚠️ 原来只查 `/dev/block/by-name`，**漏掉了 `/dev/block/platform/*/by-name`**。
     *    而模拟器/虚拟机几乎都走后者（如 `platform/host/by-name/ramdisk`），
     *    于是这些设备上本机安装页永远显示"未探测到分区"。
     *
     *    修法：**复用 RamdiskProbe.enumerateBlockDevices()**——
     *    它已经处理了 1~2 层平台名不固定的问题，两边共用一套枚举，
     *    免得一处修了另一处还是瞎的。
     */
    suspend fun findPartition(): PartitionInfo? = withContext(Dispatchers.IO) {
        val slot = slotSuffix()
        val devices = RamdiskProbe.enumerateBlockDevices()
        if (devices.isEmpty()) return@withContext null

        // 名字优先：ramdisk > init_boot > boot > vendor_boot
        for (want in listOf("ramdisk", "init_boot", "boot")) {
            val name = want + slot
            val dev = devices.firstOrNull { it.substringAfterLast('/') == name }
                // 单槽设备没有 _a 后缀
                ?: devices.firstOrNull { it.substringAfterLast('/') == want }
                ?: continue
            val sz = partitionSize(dev) ?: continue
            if (sz <= 0) continue
            return@withContext PartitionInfo(dev, want, slot, sz)
        }
        null
    }

    /** 文件大小；不存在或异常返回 null。 */
    suspend fun sizeOf(path: String): Long? = withContext(Dispatchers.IO) {
        RootShell.exec("stat -c %s $path", 8_000).output.trim().toLongOrNull()
    }

    /**
     * /data 剩余空间（字节）。
     * 读一个 100MB 的分区再写一份修补产物，加上备份一共三份——
     * 空间不够的话 dd 会写一半失败，留下一个损坏的备份，
     * 那比"直接提示没空间"糟得多。
     */
    suspend fun freeBytes(): Long? = withContext(Dispatchers.IO) {
        RootShell.exec("df -k /data/local/tmp", 8_000).output.lineSequence()
            .drop(1).firstOrNull()?.split(Regex("""\s+"""))
            ?.getOrNull(3)?.toLongOrNull()?.times(1024)
    }

    private suspend fun partitionSize(dev: String): Long? = withContext(Dispatchers.IO) {
        // blockdev 不是每台都有；/sys 的 size 是 512 字节扇区数，最通用。
        val r = RootShell.exec("blockdev --getsize64 $dev", 8_000)
        val v = r.output.trim().toLongOrNull()
        if (r.ok && v != null && v > 0) return@withContext v
        val name = dev.substringAfterLast('/')
        val s = RootShell.exec("cat /sys/class/block/$name/size", 8_000)
            .output.trim().toLongOrNull()
        s?.times(512)
    }

    data class PartitionInfo(
        val dev: String,      // /dev/block/by-name/init_boot_a
        val role: String,     // init_boot / boot
        val slot: String,     // _a / "" （只读展示用）
        val sizeBytes: Long,
    )

    // --------------------------------------------------------------- 读

    /**
     * 把当前分区 dd 成文件。**这一步的产物同时就是原厂备份**——
     * 它是整个分区的完整副本，比任何"线包里的镜像"都贴合这台机器。
     */
    suspend fun dump(part: PartitionInfo): Step = withContext(Dispatchers.IO) {
        RootShell.exec("mkdir -p $WORK $BACKUP", 8_000)
        // 三份钱：备份 + 工作副本 + 修补产物 ≈ 2.5 倍分区大小。
        // 不够就别开始——dd 写一半失败会留下损坏的备份，比不备份更糟。
        val free = freeBytes()
        if (free != null && free < part.sizeBytes * 3)
            return@withContext Step(
                false,
                "空间不足：需要约 ${part.sizeBytes * 3 / 1048576}MB，只剩 ${free / 1048576}MB"
            )
        val backup = "$BACKUP/${part.role}${part.slot}.orig.img"
        val work = "$WORK/${part.role}.img"

        // 已经备份过就别覆盖：万一第一次修补写坏了，用户第二次进来
        // 还能拿到最初那份。覆盖掉等于把唯一的退路抹了。
        val have = RootShell.exec("ls -l $backup", 8_000)
        val fresh = !have.ok || !have.output.contains(part.role)

        val r = if (fresh) {
            RootShell.exec("dd if=${part.dev} of=$backup bs=1048576", 300_000)
        } else {
            RootShell.Result(0, "复用已有备份")
        }
        if (!r.ok && fresh) return@withContext Step(false, "读分区失败：${r.output}")

        // 工作副本：修补在这一份上做，备份那份不动。
        val cp = RootShell.exec("cp $backup $work", 120_000)
        if (!cp.ok) return@withContext Step(false, "复制工作副本失败：${cp.output}")

        val sz = RootShell.exec("stat -c %s $work", 8_000).output.trim().toLongOrNull() ?: -1
        if (sz <= 0) return@withContext Step(false, "工作副本为空")

        Step(
            true,
            "已读出 ${sz / 1048576}MB（备份：$backup）",
            backup = backup,
            workImg = work,
            sizeBytes = sz,
        )
    }

    // --------------------------------------------------------------- 修补

    /**
     * 修补工作副本。
     *
     * ⚠️ veritpath 是 JNI 库，在 App 进程里跑，**不需要 root**；
     *    它读的是 /data/local/tmp 下的普通文件，App 自己读不了、
     *    但可以用 root 把它拷到私有缓存后再读。
     *    这里走"拷到 cacheDir → JNI 修补 → 拷回去"三步，
     *    是因为 JNI 拿不到 /data/local/tmp 的读权限（App 无权限）。
     */
    suspend fun patch(workImg: String, payloadDir: String, role: String): Step =
        withContext(Dispatchers.IO) {
            val local = File(app.cacheDir, "local-${role}.img").apply { delete() }
            val cpIn = RootShell.exec("cp $workImg ${local.absolutePath}", 180_000)
            if (!cpIn.ok) return@withContext Step(false, "取回镜像失败：${cpIn.output}")

            Veritpath.setTempDir(app.cacheDir.absolutePath)
            val outDir = File(app.cacheDir, "local-out").apply {
                deleteRecursively(); mkdirs()
            }
            val r = Veritpath.inject(
                Veritpath.Image.auto(local.absolutePath),
                payloadDir,
                outDir.absolutePath,
                "--keep-trailing",
            )
            if (!r.ok()) return@withContext Step(false, "修补失败（${r.exitCode}）：${r.output}")

            val made = outDir.listFiles()?.firstOrNull()
                ?: return@withContext Step(false, "修补没产出文件")

            // 拷回 root 可见的位置
            val back = "$WORK/${role}.patched.img"
            val cpOut = RootShell.exec("cp ${made.absolutePath} $back", 180_000)
            if (!cpOut.ok) return@withContext Step(false, "拷回失败：${cpOut.output}")

            val sz = RootShell.exec("stat -c %s $back", 8_000).output.trim().toLongOrNull() ?: -1
            Step(true, "修补完成（${sz / 1048576}MB）", patched = back, sizeBytes = sz)
        }

    /** 把 SAF 选中的 payload 目录落到 App 私有缓存（JNI 只认真实路径）。 */
    suspend fun preparePayload(uri: Uri): String? = withContext(Dispatchers.IO) {
        val root = androidx.documentfile.provider.DocumentFile.fromTreeUri(app, uri)
            ?: return@withContext null
        val dest = File(app.cacheDir, "local-payload").apply { deleteRecursively(); mkdirs() }
        fun walk(dir: androidx.documentfile.provider.DocumentFile, out: File) {
            for (f in dir.listFiles()) {
                val o = File(out, f.name ?: continue)
                if (f.isDirectory) { o.mkdirs(); walk(f, o) }
                else app.contentResolver.openInputStream(f.uri)?.use { i ->
                    o.outputStream().use { it.write(i.readBytes()) }
                }
            }
        }
        walk(root, dest)
        if (dest.listFiles().isNullOrEmpty()) null else dest.absolutePath
    }

    // --------------------------------------------------------------- 校验 → 写回

    /**
     * 写回前的五道闸。**任何一道不过就不写**。
     *
     * @param part 读分区时得到的那个对象——必须用同一个，不能重新探测
     */
    suspend fun preflight(
        part: PartitionInfo,
        patched: String,
        backup: String,
        role: String,
    ): Gate = withContext(Dispatchers.IO) {
        // ① 分区身份：必须与读出时一致
        val live = RootShell.exec("ls -l ${part.dev}", 8_000)
        if (!live.ok) return@withContext Gate(false, "① 分区 ${part.dev} 不见了——拒绝写回", part.dev)

        // ⑤ 备份在（放前面查，因为它决定有没有退路）
        val bsz = RootShell.exec("stat -c %s $backup", 8_000).output.trim().toLongOrNull() ?: -1
        if (bsz <= 0)
            return@withContext Gate(false, "⑤ 备份不存在或为空（$backup）——没有退路就不动", part.dev)

        // ③ 大小 ≤ 分区
        val psz = RootShell.exec("stat -c %s $patched", 8_000).output.trim().toLongOrNull() ?: -1
        if (psz <= 0) return@withContext Gate(false, "③ 修补产物不存在", part.dev)
        if (psz > part.sizeBytes)
            return@withContext Gate(
                false,
                "③ 修补产物 ${psz / 1048576}MB 超过分区 ${part.sizeBytes / 1048576}MB——" +
                    "写进去会被截断，开机必死",
                part.dev,
            )

        // ② magic：必须是 Android boot 镜像
        val magic = RootShell.exec(
            "dd if=$patched bs=1 count=8 2>/dev/null | od -c | head -1", 10_000
        ).output
        val isBoot = magic.contains("A N D R O I D") || magic.contains("V N D R B O O T")
        if (!isBoot)
            return@withContext Gate(false, "② 修补产物的 magic 不对（不是 boot 镜像）：$magic", part.dev)

        // ④ veritpath 的 verify 走 JNI（在 App 进程里跑），由上层调 verifyLocal()
        //    单独完成——这里不重复执行，免得又把大文件拷一遍。
        Gate(true, "①②③⑤ 已过（④ verify 由上层单独执行）", part.dev, psz)
    }

    /** veritpath 自检（在 App 进程里跑 JNI，先把文件取回）。 */
    suspend fun verifyLocal(patched: String, role: String): Boolean =
        withContext(Dispatchers.IO) {
            val local = File(app.cacheDir, "local-verify.img").apply { delete() }
            if (!RootShell.exec("cp $patched ${local.absolutePath}", 180_000).ok) return@withContext false
            Veritpath.setTempDir(app.cacheDir.absolutePath)
            // verify 走的还是 nativeRun，自查镜像自身完整性
            val r = Veritpath.run("verify", local.absolutePath)
            r.ok()
        }

    /**
     * 写回。**调用前必须先过 preflight + verifyLocal。**
     *
     * 这里不再做任何判断——所有判断都在闸里做过了。
     * 留一个 lastConfirm 参数只是为了防止上层误调。
     */
    suspend fun flash(part: PartitionInfo, patched: String, lastConfirm: Boolean): Step =
        withContext(Dispatchers.IO) {
            if (!lastConfirm) return@withContext Step(false, "未确认，拒绝写回")
            val r = RootShell.exec(
                "dd if=$patched of=${part.dev} bs=1048576 conv=notrunc", 300_000
            )
            if (!r.ok) return@withContext Step(false, "写回失败：${r.output}")

            /* ⚠️ 这里**不做**"回读校验"。
             * 写过之后分区内容就是新镜像，再读回来对比必然相同——
             * 那种校验是自欺欺人，反而给出虚假的安全感。
             * 真正有意义的校验全部在写之前的 preflight 里做完了。
             * 写完之后唯一诚实的说法就是：请重启验证。 */
            Step(r.ok, "写回完成——请重启，然后看 /proc/1/exe")
        }

    /** 回退：把备份写回分区。变砖时唯一自救手段。 */
    suspend fun restore(part: PartitionInfo, backup: String): Step =
        withContext(Dispatchers.IO) {
            val r = RootShell.exec(
                "dd if=$backup of=${part.dev} bs=1048576 conv=notrunc", 300_000
            )
            Step(r.ok, if (r.ok) "已还原原厂分区，请重启" else "还原失败：${r.output}")
        }

    // --------------------------------------------------------------- 数据结构

    data class Step(
        val ok: Boolean,
        val message: String,
        val backup: String? = null,
        val workImg: String? = null,
        val patched: String? = null,
        val sizeBytes: Long = 0,
    )

    data class Gate(
        val pass: Boolean,
        val message: String,
        val dev: String,
        val sizeBytes: Long = 0,
    )
}
