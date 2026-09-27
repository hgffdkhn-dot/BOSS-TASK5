package com.boss.manager.data

/** BOSS 的整体装态。首页那一张卡就是它。 */
data class BossStatus(
    val installed: Boolean,          // bossd 探活成功
    val hijacked: Boolean,           // /proc/1/exe 指向 BOSS（劫持成没成）
    val version: String,
    val protoVersion: Int,
    val selinux: String,             // enforcing / permissive / disabled
    val managerUid: Int?,            // App 自己被认定的 uid；null = 还没注册
    val moduleCount: Int,
    val bossMounts: Int,
)

/** 无修改系统逻辑（任务5 A 面）的自检结果。 */
data class SystemlessStatus(
    val configured: Boolean,
    val propEntries: Int,
    val denyEntries: Int,
    val bossMounts: Int,
    val tmpfsOverlays: Int,
    val totalMounts: Int,
)

/** `boss systemless verify` 的四项 + 结论。 */
data class VerifyReport(
    val bossMounts: Int,
    val outOfRange: List<String>,
    val readOnlyParts: List<Pair<String, Boolean>>,   // 分区 -> 是否仍为 ro
    val baselineDiff: Int?,                           // null = 无基线
    val tracePaths: List<String>,
    val conclusion: String,
    val clean: Boolean,
) {
    /** 差异不定罪：模块挂载本来就会让系统分区"看起来"变了。
     *  定罪要看挂载源，把差异直接判成失败会在装了模块的机器上天天误报。 */
    val verdictLabel
        get() = when {
            outOfRange.isNotEmpty() -> "越界 ${outOfRange.size} 处"
            clean -> "干净"
            else -> "有差异（需看挂载源判定）"
        }
}

/** 一条 BOSS 引入的挂载 = 一个暴露面。 */
data class MountExposure(
    val type: String,
    val src: String,
    val target: String,
)

data class ModuleInfo(
    val id: String,
    val name: String,
    val version: String,
    val author: String,
    val description: String,
    val enabled: Boolean,      // mount 列不是 off
    val scriptOnly: Boolean,   // mount 列是 script
)

data class PolicySnapshot(
    val path: String,
    val defaultDecision: String,
    val logEnabled: Boolean,
    val managerRegistered: Boolean,
    val rules: List<String>,
)

/** 审计日志里的一条授权记录。 */
data class AuditEntry(
    val time: String,
    val uid: Int,
    val pid: Int,
    val caller: String,
    val targetUid: Int,
    val command: String,
    val decision: String,
)

/** 待用户裁决的授权请求（任务6 的弹窗）。 */
data class PromptRequest(
    val id: String,
    val uid: Int,
    val caller: String,
    val command: String,
    val ts: String,
)
