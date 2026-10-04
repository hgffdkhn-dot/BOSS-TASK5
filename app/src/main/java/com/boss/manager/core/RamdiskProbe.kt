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

    /**
     * 候选位置。**先精确后宽泛**：
     * 前面几个是各模拟器/管理器实际在用的路径，最后的目录是兜底扫描。
     */
    private val CANDIDATES = listOf(
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
     * 扫一遍候选位置，返回第一个**确实含 ramdisk** 的。
     *
     * 不用"第一个存在的"——目录里可能有个空的 boot.img 占位文件，
     * 那种应该继续找，而不是报"找到了"。
     */
    suspend fun find(): Found? = withContext(Dispatchers.IO) {
        for (p in CANDIDATES) {
            val f = classify(p)
            if (f != null && f.kind.hasRamdisk) return@withContext f
        }
        null
    }

    /** 全扫一遍，返回所有含 ramdisk 的（供"让用户挑"用）。 */
    suspend fun findAll(): List<Found> = withContext(Dispatchers.IO) {
        CANDIDATES.mapNotNull { classify(it) }.filter { it.kind.hasRamdisk }
    }

    /** 对单个文件做判定。不存在/读不到返回 null。 */
    suspend fun classify(path: String): Found? = withContext(Dispatchers.IO) {
        val size = sizeOf(path) ?: return@withContext null
        if (size <= 0) return@withContext null
        val head = readHead(path, 8) ?: return@withContext null
        Found(path, kindOf(head, path), size)
    }

    // ------------------------------------------------------------ magic 判定

    /**
     * @param head 至少 8 字节
     * @param path 仅用于 gzip 时解压确认，失败就退回 GZIP_CPIO 的"不确定"态
     */
    private fun kindOf(head: ByteArray, path: String): Kind {
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
            return if (gzipHeadIsCpio(path)) Kind.GZIP_CPIO else Kind.UNKNOWN
        }
        if (hx(0, 4) == "04224d18" || hx(0, 4) == "02214c18") return Kind.LZ4_CPIO
        if (hx(0, 6) == "fd377a585a00") return Kind.XZ_CPIO
        if (hx(0, 3) == "425a68") return Kind.BZIP2_CPIO
        return Kind.UNKNOWN
    }

    /** 解压前几字节确认是不是 cpio。失败按"不确定"处理，不硬说它是。 */
    private fun gzipHeadIsCpio(path: String): Boolean = runCatching {
        val src = if (File(path).canRead()) File(path).inputStream()
        else RootShell.openRead(path) ?: return false
        src.use { raw ->
            java.util.zip.GZIPInputStream(raw).use { gz ->
                val b = ByteArray(6)
                val n = gz.read(b)
                n == 6 && (String(b, Charsets.US_ASCII) == "070701" ||
                        String(b, Charsets.US_ASCII) == "070702")
            }
        }
    }.getOrDefault(false)

    // ------------------------------------------------------------ 读文件

    private fun sizeOf(path: String): Long? = runCatching {
        val f = File(path)
        if (f.canRead()) f.length()
        else RootShell.sizeOfSync(path)
    }.getOrNull()

    /** 读前 n 字节。App 读得到就直接读，否则走 root。 */
    private fun readHead(path: String, n: Int): ByteArray? = runCatching {
        val f = File(path)
        if (f.canRead()) {
            f.inputStream().use { it.readNBytes(n) }
        } else {
            val hex = RootShell.headHexSync(path, n) ?: return null
            hexToBytes(hex)
        }
    }.getOrNull()

    private fun hexToBytes(hex: String): ByteArray {
        val clean = hex.filter { it.isDigit() || it in 'a'..'f' || it in 'A'..'F' }
        val out = ByteArray(clean.length / 2)
        for (i in out.indices) out[i] = clean.substring(i * 2, i * 2 + 2).toInt(16).toByte()
        return out
    }
}
