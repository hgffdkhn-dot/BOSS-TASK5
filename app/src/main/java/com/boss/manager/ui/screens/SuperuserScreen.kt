package com.boss.manager.ui.screens

import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.boss.manager.data.AuditEntry
import com.boss.manager.data.PolicySnapshot
import com.boss.manager.data.PromptRequest
import com.boss.manager.ui.BossViewModel
import com.boss.manager.ui.KV
import com.boss.manager.ui.SectionCard

/**
 * 授权页：待裁决的弹窗 + 策略 + 审计日志。
 *
 * 三元语义按任务2 的原样保留：allow / deny / prompt。
 * 变的只有 prompt——以前它"按拒绝处理"，现在真的等用户点一下。
 */
@Composable
fun SuperuserScreen(vm: BossViewModel) {
    var tab by remember { mutableIntStateOf(0) }
    Column(Modifier.fillMaxSize()) {
        // M3 里 TabRow 已拆成 PrimaryTabRow / SecondaryTabRow，老的 TabRow 弃用了。
        // 这是主导航级标签，用 PrimaryTabRow（次级用 SecondaryTabRow）。
        PrimaryTabRow(selectedTabIndex = tab) {
            listOf("待裁决", "策略", "审计日志").forEachIndexed { i, t ->
                Tab(selected = i == tab, onClick = { tab = i }, text = { Text(t) })
            }
        }
        when (tab) {
            0 -> PromptPane(vm)
            1 -> PolicyPane(vm)
            2 -> AuditPane(vm)
        }
    }
}

@Composable
private fun PromptPane(vm: BossViewModel) {
    val prompts by vm.prompts.collectAsStateWithLifecycle()
    Column(Modifier.fillMaxSize().padding(vertical = 8.dp)) {
        if (prompts.isEmpty()) {
            SectionCard("待裁决") {
                Text("（无）App 在前台时每 3 秒问一次 daemon。",
                    style = MaterialTheme.typography.bodyMedium)
            }
        } else {
            prompts.forEach { PromptCard(vm, it) }
        }
    }
}

@Composable
private fun PromptCard(vm: BossViewModel, p: PromptRequest) {
    var remember by remember { mutableStateOf(false) }
    Card(Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 6.dp)) {
        Column(Modifier.padding(16.dp)) {
            Text(p.caller.ifBlank { "uid=${p.uid}" }, fontWeight = FontWeight.Bold)
            Spacer(Modifier.height(4.dp))
            KV("uid", p.uid.toString())
            KV("命令", p.command.ifBlank { "（交互式 shell）" })
            Spacer(Modifier.height(8.dp))
            Row(verticalAlignment = androidx.compose.ui.Alignment.CenterVertically) {
                Checkbox(checked = remember, onCheckedChange = { remember = it })
                Text("记住这个应用（写一条真正的策略规则）",
                    style = MaterialTheme.typography.bodySmall)
            }
            Spacer(Modifier.height(8.dp))
            Row {
                Button(onClick = { vm.answer(p, true, remember) }) { Text("允许") }
                Spacer(Modifier.width(12.dp))
                OutlinedButton(onClick = { vm.answer(p, false, remember) }) { Text("拒绝") }
            }
            Text(
                "超时（60 秒没人点）或调用方提前退出，一律按拒绝处理——" +
                "弹窗这条路上宁可让用户重按一次，也不能默认放行。",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

@Composable
private fun PolicyPane(vm: BossViewModel) {
    val policy by vm.policy.collectAsStateWithLifecycle()
    Column(Modifier.fillMaxSize().padding(vertical = 8.dp)) {
        SectionCard("策略") {
            if (policy == null) Text("读取中…") else {
                val p = policy!!
                KV("默认", p.defaultDecision)
                KV("日志", if (p.logEnabled) "开" else "关")
                KV("manager", if (p.managerRegistered) "已注册" else "未注册",
                    warn = !p.managerRegistered)
                Spacer(Modifier.height(8.dp))
                Row {
                    TextButton(onClick = { vm.policySetDefault("deny") }) { Text("默认拒绝") }
                    Spacer(Modifier.width(8.dp))
                    TextButton(onClick = { vm.policySetDefault("allow") }) { Text("默认允许") }
                }
                Row {
                    TextButton(onClick = { vm.policySetLog(true) }) { Text("开日志") }
                    Spacer(Modifier.width(8.dp))
                    TextButton(onClick = { vm.policySetLog(false) }) { Text("关日志") }
                }
                Spacer(Modifier.height(8.dp))
                Text("规则（${p.rules.size}）", style = MaterialTheme.typography.titleSmall)
                p.rules.forEach { Text("  $it", style = MaterialTheme.typography.bodySmall) }
            }
        }
    }
}

@Composable
private fun AuditPane(vm: BossViewModel) {
    val audit by vm.audit.collectAsStateWithLifecycle()
    LaunchedEffect(Unit) { vm.refreshAudit() }
    Column(Modifier.fillMaxSize().padding(vertical = 8.dp)) {
        SectionCard("审计日志（最近 ${audit.size} 条，倒序）") {
            Row {
                TextButton(onClick = { vm.refreshAudit() }) { Text("刷新") }
                Spacer(Modifier.width(8.dp))
                Text(
                    "App 自己的状态轮询带 NOLOG，不会把这里刷满。",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            Spacer(Modifier.height(8.dp))
            if (audit.isEmpty()) {
                Text("（无记录）", style = MaterialTheme.typography.bodyMedium)
            } else {
                audit.take(80).forEach { AuditRow(it) }
            }
        }
    }
}

@Composable
private fun AuditRow(e: AuditEntry) {
    Column(Modifier.fillMaxWidth().padding(vertical = 3.dp)) {
        Text("${e.time}  ${e.caller.ifBlank { "uid=${e.uid}" }} → ${e.decision}",
            style = MaterialTheme.typography.bodySmall)
        Text("    ${e.command.ifBlank { "<interactive>" }}",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
    HorizontalDivider()
}
