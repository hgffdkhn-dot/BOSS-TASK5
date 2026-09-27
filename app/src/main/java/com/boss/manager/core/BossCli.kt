package com.boss.manager.core

/**
 * BOSS 的 CLI 契约层。
 *
 * 任务5 交接文档的原话是"文件即契约"：App 不需要新协议，
 * `/data/adb/boss/` 下那几个文件加上这套 CLI 就是读写面。
 * 这里把每个子命令封成一个方法，好处有两个：
 *   1. 命令字符串只写一遍。写散在 UI 里的话，改一处漏一处，
 *      症状是"某个页面一直在调一个早就不存在的参数"。
 *   2. 轮询类命令统一带 NOLOG。App 自己每隔几秒刷一次状态，
 *      这些请求不该进审计日志——日志是给用户看"谁拿了 root"的，
 *      被自己的轮询刷满就等于没有日志。
 *
 * 命令的真实输出格式在 tools/contract_test.sh 里有断言：
 * 改了上游的 printf，那个脚本会红——它是 App 解析器的护栏。
 */
object BossCli {

    /** 单二进制多入口。App 调的是 /data 下这份，ramdisk 里的 /boss 开机后就消失了。 */
    const val BOSS = "/data/adb/boss/boss"
    const val DIR = "/data/adb/boss"

    /** 轮询类：不记日志，超时短一点，UI 不等人 */
    private fun query(args: String, timeoutMs: Int = 10_000): BossIpc.Result =
        BossIpc.exec("$BOSS $args", flags = BossIpc.Flag.NOLOG, timeoutMs = timeoutMs)

    /** 变更类：要记进审计日志（用户需要看到"谁改了什么"） */
    private fun mutate(args: String, timeoutMs: Int = 20_000): BossIpc.Result =
        BossIpc.exec("$BOSS $args", flags = 0, timeoutMs = timeoutMs)

    // ---- 探活与版本 ----
    fun ping(): Boolean = BossIpc.ping()
    fun version(): BossIpc.Result = query("version")

    // ---- 无修改系统逻辑（任务5 A 面）----
    fun systemlessStatus() = query("systemless status")
    fun systemlessVerify(saveBaseline: Boolean = false) =
        query("systemless verify" + if (saveBaseline) " --save" else "")
    fun systemlessPlan() = query("systemless plan")
    fun systemlessApply(stage: String = "service") = mutate("systemless apply --stage $stage")

    // ---- 特典（任务5 B 面）----
    fun hideDenyList() = query("hide denylist list")
    fun hideDenyAdd(pkg: String) = mutate("hide denylist add ${shq(pkg)}")
    fun hideDenyDel(pkg: String) = mutate("hide denylist del ${shq(pkg)}")
    fun hideMounts() = query("hide mounts")
    fun hideScan() = query("hide scan", timeoutMs = 20_000)
    fun hideUmount(pid: Int, dry: Boolean = true) =
        mutate("hide umount $pid" + if (dry) " --dry" else "")
    fun hidePropsTemplate() = mutate("hide props --save-template")

    // ---- 模块（任务3）----
    fun moduleList() = query("module list")
    fun moduleInfo(id: String) = query("module info ${shq(id)}")
    fun modulePlan() = query("module plan", timeoutMs = 20_000)
    fun moduleMount() = mutate("module mount", timeoutMs = 30_000)

    // ---- 授权策略（任务2）----
    fun policyShow() = query("policy show")
    fun policySetDefault(decision: String) = mutate("policy set default $decision")
    fun policySetLog(on: Boolean) = mutate("policy set log ${if (on) 1 else 0}")
    fun policyAdd(kind: String, id: Int, decision: String) = mutate("policy add $kind $id $decision")
    fun policyDel(kind: String, id: Int) = mutate("policy del $kind $id")
    fun policyManager(uid: Int? = null) = mutate("policy manager ${uid ?: ""}".trim())

    // ---- UI 控制通道（任务6 新增）----
    fun uiManager() = BossIpc.exec("manager", flags = BossIpc.Flag.UI or BossIpc.Flag.NOLOG)
    fun uiPending() = BossIpc.exec("pending", flags = BossIpc.Flag.UI or BossIpc.Flag.NOLOG,
        timeoutMs = 10_000)
    fun uiAnswer(id: String, allow: Boolean) =
        BossIpc.exec("${if (allow) "allow" else "deny"} $id",
            flags = BossIpc.Flag.UI, timeoutMs = 10_000)

    // ---- 开机编排与工具（任务3/4）----
    fun bootStage(stage: String) = mutate("boot $stage", timeoutMs = 60_000)
    fun selinuxStatus() = query("selinux status")
    fun appletInstall() = mutate("applet install", timeoutMs = 30_000)
    fun resetprop(name: String) = query("resetprop $name")

    // ---- 日志 ----
    fun tailLog(lines: Int = 300) =
        BossIpc.exec("/system/bin/tail -n $lines $DIR/boss.log",
            flags = BossIpc.Flag.NOLOG, timeoutMs = 10_000)

    /** 单引号包裹并转义内嵌单引号：包名来自用户输入，直接拼会变成命令注入。 */
    private fun shq(s: String): String = "'" + s.replace("'", "'\\''") + "'"
}
