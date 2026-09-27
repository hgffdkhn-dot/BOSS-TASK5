package com.boss.manager.data

/**
 * CLI 输出解析器。
 *
 * 全部是纯函数（字符串进、对象出），不碰 IPC——这样它们能被单独验。
 *
 * ⚠️ 解析规则必须与上游 printf 一一对应，护栏是 tools/contract_test.sh：
 *    它跑真实的 boss 二进制，断言这里依赖的每一行标记都还在。
 *    上游改了输出格式而这里没跟上，表现不是崩溃，而是"页面永远显示 0 条"，
 *    这类 bug 在真机上极难定位——所以那份自检不能删。
 *
 * 一个通用经验：这里的匹配全是"找标记 + 取数字"，没有一处依赖行号。
 * 上游加一行说明文字就会让按行号解析的版本全部错位。
 */
object Parsers {

    private val WS = Regex("\\s+")

    fun systemlessStatus(out: String): SystemlessStatus {
        val configured = out.contains("：已配置")
        val counts = Regex("属性条目\\s*(\\d+)，隐藏条目\\s*(\\d+)").find(out)
        val mounts = Regex("BOSS 引入挂载：(\\d+) 条（其中 tmpfs 覆盖层 (\\d+) 条）").find(out)
        val total = Regex("挂载表总条数：(\\d+)").find(out)
        return SystemlessStatus(
            configured = configured,
            propEntries = counts?.groupValues?.get(1)?.toIntOrNull() ?: 0,
            denyEntries = counts?.groupValues?.get(2)?.toIntOrNull() ?: 0,
            bossMounts = mounts?.groupValues?.get(1)?.toIntOrNull() ?: 0,
            tmpfsOverlays = mounts?.groupValues?.get(2)?.toIntOrNull() ?: 0,
            totalMounts = total?.groupValues?.get(1)?.toIntOrNull() ?: 0,
        )
    }

    fun verifyReport(out: String): VerifyReport {
        val outOfRange = Regex("^\\s*\\[越界]\\s*(.+)$", RegexOption.MULTILINE)
            .findAll(out).map { it.groupValues[1].trim() }.toList()

        val roParts = mutableListOf<Pair<String, Boolean>>()
        Regex("^\\s{3}(\\S+)\\s+(ro|rw)（.*）$", RegexOption.MULTILINE)
            .findAll(out).forEach { m ->
                roParts += m.groupValues[1] to (m.groupValues[2] == "ro")
            }

        // 三种写法都要认：一致 / 有 N 处差异 / 无基线
        val diff = Regex("与基线有 (\\d+) 处差异").find(out)?.groupValues?.get(1)?.toIntOrNull()
        val hasBaseline = !out.contains("无基线")
        val baselineDiff = if (hasBaseline) (diff ?: 0) else null

        val traces = Regex("^\\s*存在：(\\S+)", RegexOption.MULTILINE)
            .findAll(out).map { it.groupValues[1] }.toList()

        val conclusion = Regex("^结论：(.+)$", RegexOption.MULTILINE)
            .find(out)?.groupValues?.get(1)?.trim() ?: ""

        val mounts = Regex("BOSS 挂载 (\\d+) 条，越界 (\\d+) 处").find(out)
        return VerifyReport(
            bossMounts = mounts?.groupValues?.get(1)?.toIntOrNull() ?: 0,
            outOfRange = outOfRange,
            readOnlyParts = roParts,
            baselineDiff = baselineDiff,
            tracePaths = traces,
            conclusion = conclusion,
            clean = conclusion.contains("干净"),
        )
    }

    /**
     * `boss hide mounts` 的行格式是 `  %-6s %-46s -> %s`。
     * 按 "->" 拆最稳：源路径里可能有空格（模块目录常见）。
     */
    fun mountExposures(out: String): List<MountExposure> {
        val list = mutableListOf<MountExposure>()
        for (line in out.lineSequence()) {
            val t = line.trim()
            if (!t.startsWith("  ")) continue
            val arrow = t.indexOf("-> ")
            if (arrow < 0) continue
            val left = t.substring(0, arrow).trim()
            val target = t.substring(arrow + 3).trim()
            val parts = left.split(WS, limit = 2)
            val type = parts.getOrNull(0) ?: continue
            val src = parts.getOrNull(1) ?: continue
            list += MountExposure(type, src, target)
        }
        return list
    }

    /** denylist list 的输出是一行一个名字，没有表头也没有计数。 */
    fun denyList(out: String): List<String> =
        out.lineSequence().map { it.trim() }
            .filter { it.isNotEmpty() && !it.startsWith("#") }
            .distinct().toList()

    /** module list：表头以 ID 起头；列宽是 printf 的 %-24s %-10s %-6s %s。 */
    fun modules(out: String): List<ModuleInfo> {
        val list = mutableListOf<ModuleInfo>()
        for (line in out.lineSequence()) {
            val t = line.trimEnd()
            if (t.isBlank() || t.startsWith("ID") || t.startsWith("(")) continue
            val m = Regex("^(\\S+)\\s+(\\S+)\\s+(\\S+)\\s*(.*)$").find(t) ?: continue
            val mount = m.groupValues[3]
            list += ModuleInfo(
                id = m.groupValues[1],
                version = m.groupValues[2].takeUnless { it == "-" } ?: "",
                enabled = mount != "off",
                scriptOnly = mount == "script",
                name = m.groupValues[4].ifBlank { m.groupValues[1] },
                author = "",
                description = "",
            )
        }
        return list
    }

    fun moduleInfo(out: String): ModuleInfo? {
        fun v(key: String) = Regex("^$key:\\s*(.*)$", RegexOption.MULTILINE)
            .find(out)?.groupValues?.get(1)?.trim().orEmpty()
        val id = v("id")
        if (id.isEmpty()) return null
        val mount = v("mount")
        return ModuleInfo(
            id = id,
            name = v("name"),
            version = v("version"),
            author = v("author"),
            description = v("desc"),
            enabled = mount != "disabled",
            scriptOnly = mount == "script-only",
        )
    }

    fun policy(out: String): PolicySnapshot {
        fun v(key: String) = Regex("^$key\\s*:\\s*(.*)$", RegexOption.MULTILINE)
            .find(out)?.groupValues?.get(1)?.trim().orEmpty()
        return PolicySnapshot(
            path = v("path"),
            defaultDecision = v("default"),
            logEnabled = v("log") == "1",
            managerRegistered = !v("manager").contains("未注册"),
            rules = Regex("^rule\\s*:\\s*(.+)$", RegexOption.MULTILINE)
                .findAll(out).map { it.groupValues[1].trim() }.toList(),
        )
    }

    /**
     * 审计日志：`[时间] uid=.. pid=.. caller='..' target=.. cmd='..' ctx='..' -> allow`
     * cmd 里可能有空格甚至 '->'，所以从右往左取最后一个 " -> "。
     */
    fun audit(out: String): List<AuditEntry> {
        val list = mutableListOf<AuditEntry>()
        for (line in out.lineSequence()) {
            if (!line.startsWith("[")) continue
            val time = line.substringAfter("[").substringBefore("]")
            val rest = line.substringAfter("] ").trim()
            val decision = rest.substringAfterLast(" -> ", "").trim()
            fun num(k: String) = Regex("$k=(\\d+)").find(rest)?.groupValues?.get(1)?.toIntOrNull() ?: -1
            fun str(k: String) = Regex("$k='([^']*)'").find(rest)?.groupValues?.get(1) ?: ""
            list += AuditEntry(
                time = time, uid = num("uid"), pid = num("pid"),
                caller = str("caller"), targetUid = num("target"),
                command = str("cmd"), decision = decision,
            )
        }
        return list
    }

    /** UI 通道的 pending：TSV，cmd 在最后、整行剩下的都算它（它可能含制表符）。 */
    fun prompts(out: String): List<PromptRequest> {
        val list = mutableListOf<PromptRequest>()
        for (line in out.lineSequence()) {
            if (line.isBlank()) continue
            val f = line.split("\t")
            if (f.size < 4) continue
            list += PromptRequest(
                id = f[0],
                uid = f[1].toIntOrNull() ?: -1,
                caller = f[2],
                command = f[3],
                ts = f.getOrNull(4).orEmpty(),
            )
        }
        return list
    }
}
