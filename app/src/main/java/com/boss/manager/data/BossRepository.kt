package com.boss.manager.data

import com.boss.manager.core.BossCli
import com.boss.manager.core.BossIpc
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * UI 与 BOSS 之间唯一的取数口。
 *
 * 所有 IPC 都切到 Dispatchers.IO：抽象套接字的读写是阻塞的，
 * 放在主线程上会直接 ANR——尤其弹窗那条路，daemon 会真的等用户点。
 */
class BossRepository {

    suspend fun isUp(): Boolean = withContext(Dispatchers.IO) { BossCli.ping() }

    suspend fun status(): BossStatus = withContext(Dispatchers.IO) {
        val up = BossCli.ping()
        val ver = BossCli.version().output.trim()
        // 劫持判定：/proc/1/exe 指向 BOSS = 成功。
        // 这是任务5 交接文档里"接手一台没验过的机器，第一件事"——
        // 它比翻日志快得多，那个阶段日志可能一个字节都没有。
        val exe = BossIpc.exec("/system/bin/readlink /proc/1/exe",
            flags = BossIpc.Flag.NOLOG, timeoutMs = 5_000).output.trim()
        val selinux = BossIpc.exec("/system/bin/getenforce",
            flags = BossIpc.Flag.NOLOG, timeoutMs = 5_000).output.trim()
        val modules = Parsers.modules(BossCli.moduleList().output)
        val sl = Parsers.systemlessStatus(BossCli.systemlessStatus().output)
        BossStatus(
            installed = up,
            hijacked = exe.contains("boss"),
            version = ver.substringAfter("BOSS ").substringBefore(" ").ifEmpty { ver },
            protoVersion = Regex("proto v(\\d+)").find(ver)?.groupValues?.get(1)?.toIntOrNull() ?: 0,
            selinux = selinux.ifEmpty { "unknown" },
            managerUid = Regex("manager : (\\d+)").find(BossCli.policyManager().output)
                ?.groupValues?.get(1)?.toIntOrNull(),
            moduleCount = modules.size,
            bossMounts = sl.bossMounts,
        )
    }

    suspend fun systemless(): SystemlessStatus = withContext(Dispatchers.IO) {
        Parsers.systemlessStatus(BossCli.systemlessStatus().output)
    }

    suspend fun verify(saveBaseline: Boolean = false): VerifyReport = withContext(Dispatchers.IO) {
        Parsers.verifyReport(BossCli.systemlessVerify(saveBaseline).output)
    }

    suspend fun exposures(): List<MountExposure> = withContext(Dispatchers.IO) {
        Parsers.mountExposures(BossCli.hideMounts().output)
    }

    suspend fun denyList(): List<String> = withContext(Dispatchers.IO) {
        Parsers.denyList(BossCli.hideDenyList().output)
    }

    suspend fun denyAdd(pkg: String) = withContext(Dispatchers.IO) { BossCli.hideDenyAdd(pkg) }
    suspend fun denyDel(pkg: String) = withContext(Dispatchers.IO) { BossCli.hideDenyDel(pkg) }

    suspend fun modules(): List<ModuleInfo> = withContext(Dispatchers.IO) {
        Parsers.modules(BossCli.moduleList().output)
    }

    suspend fun modulePlan(): String = withContext(Dispatchers.IO) {
        BossCli.modulePlan().output
    }

    suspend fun policy(): PolicySnapshot = withContext(Dispatchers.IO) {
        Parsers.policy(BossCli.policyShow().output)
    }

    suspend fun policySetDefault(d: String) = withContext(Dispatchers.IO) { BossCli.policySetDefault(d) }
    suspend fun policySetLog(on: Boolean) = withContext(Dispatchers.IO) { BossCli.policySetLog(on) }

    suspend fun auditLog(): List<AuditEntry> = withContext(Dispatchers.IO) {
        Parsers.audit(BossCli.tailLog().output).asReversed()
    }

    /** 弹窗轮询：App 在前台时每隔几秒问一次有没有待裁决的请求。 */
    suspend fun pendingPrompts(): List<PromptRequest> = withContext(Dispatchers.IO) {
        Parsers.prompts(BossCli.uiPending().output)
    }

    suspend fun answerPrompt(id: String, allow: Boolean) = withContext(Dispatchers.IO) {
        BossCli.uiAnswer(id, allow)
    }

    /** 长期放行：本次放行只管这一次，要记住得写一条真正的策略规则。 */
    suspend fun rememberApp(uid: Int, allow: Boolean) = withContext(Dispatchers.IO) {
        val appId = uid % 100000      // Android：appId = uid % 100000，跨用户同包同 appId
        if (allow) BossCli.policyAdd("app", appId, "allow")
        else BossCli.policyDel("app", appId)
    }
}
