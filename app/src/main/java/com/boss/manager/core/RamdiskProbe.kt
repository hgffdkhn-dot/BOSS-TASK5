package com.boss.manager.core

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File

/**
 * 识别一个文件到底是 **boot 镜像** 还是 **裸 ramdisk**，顺带找它。
 *
 * ## 为什么要分这两种
 *
 * 真机上操作的是完整分区镜像（`fastboot flash init_boot`），
 * 而**模拟器 / 虚拟机 / 容器化 Android**（Waydroid、WSA、各类云手机、
 * 某些给管理器开的后门）常常直接暴露一个 **ramdisk**——没有 boot 头，
 * 就是个 cpio（可能压缩过）。
 *
 * 这两种东西的处理路径完全不同：
 *   · boot 镜像 → veritpath inject 直接改 ramdisk 段
 *   · 裸 ramdisk → 没有头可改，得走 unpack / 改 / repack
 *
 * 混为一谈的后果是：拿裸 ramdisk 去 inject，报错信息指向"镜像解析失败"，
 * 用户会以为是镜像坏了，实际上是**类型不对**。
 *
 * ## 判定依据：文件头 magic
 *
 * | magic | 是什么 |
 * |---|---|
 * | `ANDROID!` | boot / init_boot / recovery 镜像 |
 * | `VNDRBOOT` | vendor_boot 镜像 |
 * | `070701` `070702` | 裸 cpio（newc / crc） |
 * | `1f 8b` | gzip —— **会解压确认里面是不是 cpio** |
 * | `04 22 4d 18` | lz4 |
 * | `02 21 4c 18` | lz4 legacy |
 * | `fd 37 7a 58 5a 00` | xz |
 * | `42 5a 68` | bzip2 |
 *
 * ⚠️ gzip 一定要**真的解压**再判：boot 镜像的 ramdisk 段也常是 gzip，
 *    但那不影响外层仍是 boot 镜像；而一个 .cpio.gz 解压后头是 070701。
 *    只看首字节会把"压缩的裸 ramdisk"误判成未知。
 */
object RamdiskProbe {

    enum class Kind {
        BOOT,           // ANDROID!
        VENDOR_BOOT,    // VNDRBOOT
        RAW_CPIO,       // 裸 cpio，未压缩
        GZIP_CPIO,      // gzip 且解压后是 cpio
        LZ4_CPIO,       // lz4（含 legacy）
        XZ_CPIO,
        BZIP2_CPIO,
        UNKNOWN,        // 头认识但内容不对 / 都不匹配
        MISSING,        // 不存在或读不到
        ;

        /** 主界面要的"有没有 ramdisk"：boot 镜像里有，裸 cpio 本身就是。 */
        val hasRamdisk: Boolean
            get() = this != UNKNOWN && this != MISSING

        val isBootImage: Boolean get() = this == BOOT || this == VENDOR_BOOT

        val label: String
            get() = when (this) {
                BOOT -> "boot 镜像"
                VENDOR_BOOT -> "vendor_boot 镜像"
                RAW_CPIO -> "裸 cpio"
                GZIP_CPIO -> "gzip 压缩的 ramdisk"
                LZ4_CPIO -> "lz4 压缩的 ramdisk"
                XZ_CPIO -> "xz 压缩的 ramdisk"
                BZIP2_CPIO -> "bzip2 压缩的 ramdisk"
                UNKNOWN -> "格式不认识"
                MISSING -> "不存在"
            }
    }

    data class Found(
        val path: String,
        val kind: Kind,
        val sizeBytes: Long,
    ) {
        val hasRamdisk: Boolean get() = kind.hasRamdisk
    }

    /** 普通文件候选位置（块设备走下面的 by-name 枚举，不写死在这里）。 */
    private val FILE_CANDIDATES = listOf(
        "/data/local/tmp/ramdisk.cpio",
        "/data/local/tmp/ramdisk.cpio.gz",
        "/data/local/tmp/ramdisk.img",
        "/data/local/tmp/boot.img",
        "/data/local/tmp/init_boot.img",
        "/data/local/tmp/vendor_boot.img",
        "/data/adb/ramdisk.cpio",
        "/data/adb/boot.img",
        "/sdcard/ramdisk.cpio",
        "/sdcard/boot.img",
        "/sdcard/init_boot.img",
        "/sdcard/Download/boot.img",
        "/sdcard/Download/init_boot.img",
        "/tmp/ramdisk.cpio",
        "/tmp/boot.img",
    )

    /**
     * by-name 目录的根。**中间那段平台名不固定**，所以只能给根、运行时枚举：
     *
     *   /dev/block/by-name                      ← 少数设备直接建这层
     *   /dev/block/platform/<soc>/by-name       ← 常见（如 .../platform/host/by-name）
     *   /dev/block/platform/<soc>/<x>/by-name   ← 有些多一层
     *
     * ⚠️ 硬编码 "host" 是错的：那只是某一台虚拟机/模拟器上的名字，
     *    真机上是 soc 厂商名（如 soc/1d84000.ufshc）。
     */
    private val BY_NAME_ROOTS = listOf(
        "/dev/block/by-name",
        "/dev/block/platform",
    )

    /** 块设备名的优先顺序：越像 ramdisk 的越靠前。 */
    private val NAME_PRIORITY = listOf(
        "ramdisk", "init_boot", "boot", "vendor_boot", "recovery",
    )

    /** 执行 shell 的抽象。**离机测试可注入假输出**，默认走 RootShell。 */
    internal var shell: (String) -> String? = { RootShell.shSync(it) }

    /**
     * 枚举所有 by-name 目录下的块设备，按名字优先级排序。
     *
     * 返回形如 `/dev/block/platform/host/by-name/ramdisk` 的完整路径。
     */
    internal fun enumerateBlockDevices(): List<String> {
        val dirs = mutableListOf<String>()
        for (root in BY_NAME_ROOTS) {
            if (root.endsWith("/by-name")) {
                dirs += root
                continue
            }
            // platform 下可能有 1~2 层
            val lvl1 = shell("ls -1 $root")?.lineSequence()
                ?.map { it.trim() }?.filter { it.isNotEmpty() } ?: continue
            for (a in lvl1) {
                val d1 = "$root/$a/by-name"
                if (shell("test -d $d1 && echo yes")?.trim() == "yes") {
                    dirs += d1
                } else {
                    // 再下一层
                    val lvl2 = shell("ls -1 $root/$a")?.lineSequence()
                        ?.map { it.trim() }?.filter { it.isNotEmpty() } ?: continue
                    for (b in lvl2) {
                        val d2 = "$root/$a/$b/by-name"
                        if (shell("test -d $d2 && echo yes")?.trim() == "yes") dirs += d2
                    }
                }
            }
        }
        val out = mutableListOf<String>()
        for (d in dirs) {
            val names = shell("ls -1 $d")?.lineSequence()
                ?.map { it.trim() }?.filter { it.isNotEmpty() } ?: continue
            out += names.map { "$d/$it" }
        }
        // 名字优先级：ramdisk > init_boot > boot > ...
        return out.sortedWith(compareBy { p ->
            val n = p.substringAfterLast('/')
            NAME_PRIORITY.indexOfFirst { n == it || n.startsWith(it) }
                .let { if (it < 0) NAME_PRIORITY.size else it }
        })
    }

    /**
     * 扫一遍，返回第一个**确实含 ramdisk** 的。
     *
     * 顺序：**块设备 by-name 优先**，普通文件兜底。
     * 因为模拟器/虚拟机把 ramdisk 暴露成 `/dev/block/platform/<soc>/by-name/<name>`
     * 这种块设备节点，而真机刷机用的镜像反而常躺在 /sdcard 里。
     *
     * 不用"第一个存在的"——目录里可能有个空占位文件，
     * 那种应该继续找，而不是报"找到了"。
     */
    suspend fun find(): Found? = withContext(Dispatchers.IO) {
        for (p in enumerateBlockDevices()) {
            val f = classify(p)
            if (f != null && f.kind.hasRamdisk) return@withContext f
        }
        for (p in FILE_CANDIDATES) {
            val f = classify(p)
            if (f != null && f.kind.hasRamdisk) return@withContext f
        }
        null
    }

    /** 全扫一遍，返回所有含 ramdisk 的（供"让用户挑"用）。 */
    suspend fun findAll(): List<Found> = withContext(Dispatchers.IO) {
        (enumerateBlockDevices() + FILE_CANDIDATES)
            .mapNotNull { classify(it) }
            .filter { it.kind.hasRamdisk }
    }

    /** 把扫过的路径列出来——找不到时给用户看"我找过哪些地方"。 */
    fun searchedPaths(): List<String> =
        runCatching { enumerateBlockDevices() }.getOrDefault(emptyList()) + FILE_CANDIDATES

    /** 对单个路径做判定（普通文件或块设备都行）。不存在/读不到返回 null。 */
    suspend fun classify(path: String): Found? = withContext(Dispatchers.IO) {
        /* ⚠️ 这里曾经有个致命 bug：`size <= 0` 就直接 return null。
         *
         *   而**块设备的 File.length() 一律返回 0**（实测 /dev/zero、/dev/null
         *   都是 0 字节，它们根本不是常规文件）。于是 /dev/block/ 下面
         *   每一个节点都被"大小为 0"这条判据无声跳过——
         *
         *   表现：明明 ramdisk 就在 /dev/block/platform/host/by-name/ramdisk，
         *         App 却报"否"。这就是为什么必须专门修它。
         *
         *   修法：块设备改走 blockdev --getsize64 / /sys/class/block/<n>/size，
         *        并且**块设备不做 <=0 过滤**（读不到大小也要继续读头判定）。 */
        val isBlock = isBlockDevice(path)
        val size = if (isBlock) blockSize(path) else fileSize(path)
        val head = readHead(path, 8, isBlock) ?: return@withContext null
        val kind = kindOf(head, path, isBlock)
        if (!isBlock && (size == null || size <= 0L)) {
            // 普通文件为 0 字节确实是空的——那种才是真的没有
            if (size != null && size <= 0L) return@withContext null
        }
        Found(path, kind, size ?: 0L)
    }

    /** 块设备判断：/dev/block/ 下的都算，另外非常规文件也算。 */
    private fun isBlockDevice(path: String): Boolean {
        if (path.startsWith("/dev/block/")) return true
        return runCatching {
            val f = java.io.File(path)
            f.exists() && !f.isFile && !f.isDirectory
        }.getOrDefault(false)
    }

    /**
     * 块设备大小。
     * `blockdev` 不是每台都有，所以退回 /sys/class/block/<name>/size（512 字节扇区数）。
     */
    private fun blockSize(dev: String): Long? {
        val r = shell("blockdev --getsize64 $dev")
        val v = r?.trim()?.toLongOrNull()
        if (v != null && v > 0) return v
        val name = dev.substringAfterLast('/')
        val s = shell("cat /sys/class/block/$name/size")?.trim()?.toLongOrNull()
        return s?.times(512)
    }

    private fun fileSize(path: String): Long? = runCatching {
        val f = java.io.File(path)
        if (f.canRead()) f.length() else shell("stat -c %s '$path'")?.trim()?.toLongOrNull()
    }.getOrNull()

    // ------------------------------------------------------------ magic 判定

    /**
     * @param head 至少 8 字节
     * @param path 仅用于 gzip 时解压确认，失败就退回 GZIP_CPIO 的"不确定"态
     */
    private fun kindOf(head: ByteArray, path: String, isBlock: Boolean = false): Kind {
        fun eq(off: Int, s: String) =
            head.size >= off + s.length && String(head, off, s.length, Charsets.US_ASCII) == s
        fun hx(off: Int, len: Int) =
            if (head.size >= off + len) head.copyOfRange(off, off + len)
                .joinToString("") { "%02x".format(it) } else ""

        if (eq(0, "ANDROID!")) return Kind.BOOT
        if (eq(0, "VNDRBOOT")) return Kind.VENDOR_BOOT
        if (eq(0, "070701") || eq(0, "070702")) return Kind.RAW_CPIO

        if (hx(0, 2) == "1f8b") {
            // gzip：解压后看头 6 字节是不是 cpio newc
            return if (gzipHeadIsCpio(path, isBlock)) Kind.GZIP_CPIO else Kind.UNKNOWN
        }
        if (hx(0, 4) == "04224d18" || hx(0, 4) == "02214c18") return Kind.LZ4_CPIO
        if (hx(0, 6) == "fd377a585a00") return Kind.XZ_CPIO
        if (hx(0, 3) == "425a68") return Kind.BZIP2_CPIO
        return Kind.UNKNOWN
    }

    /**
     * 解压前几字节确认是不是 cpio。失败按"不确定"处理，不硬说它是。
     *
     * ⚠️ 块设备**不能**走 FileInputStream（没权限，且不是常规文件），
     *    得用 shell 管道：`dd | gzip -dc | head -c 6 | od`。
     *    gzip 流只给前 8 字节解压不出 6 个字节，所以取 512 字节——
     *    足够 inflate 出开头，又不至于把整个分区读出来。
     */
    private fun gzipHeadIsCpio(path: String, isBlock: Boolean): Boolean {
        val want = { b: ByteArray ->
            b.size >= 6 && String(b, 0, 6, Charsets.US_ASCII).let {
                it == "070701" || it == "070702"
            }
        }
        if (!isBlock) {
            runCatching {
                val f = java.io.File(path)
                if (f.canRead()) {
                    java.util.zip.GZIPInputStream(f.inputStream()).use { gz ->
                        val b = ByteArray(6)
                        if (gz.read(b) == 6 && want(b)) return true
                    }
                }
            }
        }
        val hex = shell(
            "dd if='$path' bs=1 count=512 2>/dev/null | gzip -dc 2>/dev/null | " +
                "head -c 6 | od -An -tx1"
        ) ?: return false
        val b = hexToBytes(hex)
        return want(b)
    }

    // ------------------------------------------------------------ 读文件

    /**
     * 读前 n 字节。
     *
     * ⚠️ 块设备**一律走 root**：App 对 /dev/block/ 下的节点没有读权限，
     *    而且 `canRead()` 可能返回 true 但实际读被 SELinux 拒——
     *    那种半吊子状态比直接没权限更难查。
     */
    private fun readHead(path: String, n: Int, isBlock: Boolean): ByteArray? =
        runCatching {
            if (!isBlock) {
                val f = java.io.File(path)
                if (f.canRead()) return@runCatching f.inputStream().use { it.readNBytes(n) }
            }
            val hex = shell("dd if='$path' bs=1 count=$n 2>/dev/null | od -An -tx1")
                ?: return@runCatching null
            hexToBytes(hex).takeIf { it.isNotEmpty() }
        }.getOrNull()

    /** od -An -tx1 的输出 → 字节数组。长度奇数时丢弃最后一个半字节。 */
    private fun hexToBytes(hex: String): ByteArray {
        val clean = hex.filter { it.isDigit() || it in 'a'..'f' || it in 'A'..'F' }
        val out = ByteArray(clean.length / 2)
        for (i in out.indices) out[i] = clean.substring(i * 2, i * 2 + 2).toInt(16).toByte()
        return out
    }
}
