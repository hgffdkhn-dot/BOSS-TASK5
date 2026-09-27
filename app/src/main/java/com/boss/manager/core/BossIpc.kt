package com.boss.manager.core

/**
 * BOSS App 与 bossd 之间的唯一通道。
 *
 * 设计上刻意"无特权"：这个类只是把请求送进 daemon，授权、提权、SELinux 切换
 * 全在 daemon 侧完成（架构红线第 1、2 条）。App 拿不到 setuid，也拿不到
 * 任何 capabilities——历史上 su 的漏洞几乎全出在"给客户端加特权"这一步。
 *
 * 协议细节见 app/src/main/cpp/boss_ipc.c；布局一致性由
 * tools/ipc_layout_test.c 在编译期钉死。
 */
object BossIpc {

    init {
        System.loadLibrary("bossipc")
    }

    private external fun nativePing(): Boolean

    private external fun nativeExec(
        command: String,
        shell: String?,
        context: String?,
        uid: Int,
        gid: Int,
        flags: Int,
        timeoutMs: Int
    ): String

    /** 请求标志位，必须与 src/boss.h 对齐（ipc_layout_test 会校验） */
    object Flag {
        const val LOGIN = 1 shl 0
        const val KEEPENV = 1 shl 1
        const val NOLOG = 1 shl 2   // 本次请求不进审计日志
        const val PING = 1 shl 3
        const val UI = 1 shl 4      // UI 控制通道（不 fork 子进程）
    }

    /** 响应码，与 boss.h 的 BOSS_OK/DENIED/ERR/PROMPT 对齐 */
    enum class Code(val raw: Int) {
        OK(0), DENIED(1), ERR(2), PROMPT(3), DOWN(-1);

        companion object {
            fun of(raw: Int) = entries.firstOrNull { it.raw == raw } ?: ERR
        }
    }

    /** CLI 返回码语义（任务4 定的，任务5 沿用，App 必须照着显示） */
    enum class ExitCode(val raw: Int, val label: String) {
        ALL(0, "全部应用"),
        FAIL(1, "失败"),
        NO_CAP(2, "无能力"),
        PARTIAL(3, "部分应用");

        companion object {
            fun of(raw: Int) = entries.firstOrNull { it.raw == raw }
                ?: FAIL.let { if (raw > 0) it else ALL }
        }
    }

    data class Result(
        val code: Code,
        val exitCode: Int,
        val timedOut: Boolean,
        val output: String,
    ) {
        /** 部分应用（3）不是错误：它让一条规则在某机型上失效时不至于中断开机。
         *  UI 上显示成"失败"会误导用户以为 BOSS 坏了。 */
        val isPartial get() = exitCode == ExitCode.PARTIAL.raw
        val isDenied get() = code == Code.DENIED
        val isDown get() = code == Code.DOWN
        val lines get() = output.lineSequence().filter { it.isNotBlank() }.toList()
    }

    fun ping(): Boolean = runCatching { nativePing() }.getOrDefault(false)

    fun exec(
        command: String,
        shell: String = DEFAULT_SHELL,
        context: String? = null,
        uid: Int = 0,
        gid: Int = 0,
        flags: Int = 0,
        timeoutMs: Int = DEFAULT_TIMEOUT_MS,
    ): Result {
        val raw = runCatching {
            nativeExec(command, shell, context, uid, gid, flags, timeoutMs)
        }.getOrNull() ?: return Result(Code.DOWN, -1, false, "")

        if (raw == "DOWN\n" || raw.startsWith("DOWN")) {
            return Result(Code.DOWN, -1, false, "")
        }

        // 报文头：CODE=<n> EXIT=<n> TMO=<0|1>\n
        val nl = raw.indexOf('\n')
        val header = if (nl >= 0) raw.substring(0, nl) else raw
        val body = if (nl >= 0) raw.substring(nl + 1) else ""
        val code = header.substringAfter("CODE=", "-1").substringBefore(' ').toIntOrNull() ?: -1
        val exit = header.substringAfter("EXIT=", "-1").substringBefore(' ').toIntOrNull() ?: -1
        val tmo = header.substringAfter("TMO=", "0").substringBefore(' ').toIntOrNull() == 1
        return Result(Code.of(code), exit, tmo, body)
    }

    const val DEFAULT_SHELL = "/system/bin/sh"
    const val DEFAULT_TIMEOUT_MS = 15_000
}
