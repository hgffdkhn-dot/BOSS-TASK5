package com.boss.manager.ui.screens

import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.boss.manager.data.ModuleInfo
import com.boss.manager.ui.BossViewModel
import com.boss.manager.ui.KV
import com.boss.manager.ui.SectionCard

@Composable
fun ModuleScreen(vm: BossViewModel) {
    val modules by vm.modules.collectAsStateWithLifecycle()
    var plan by remember { mutableStateOf<String?>(null) }

    Column(Modifier.fillMaxSize().padding(vertical = 8.dp)) {
        SectionCard("已安装模块（${modules.size}）") {
            if (modules.isEmpty()) {
                Text("（无模块）", style = MaterialTheme.typography.bodyMedium)
            } else {
                modules.forEach { ModuleRow(it) }
            }
        }

        SectionCard("挂载计划（plan 是 dry run，不改动系统）") {
            TextButton(onClick = { vm.toast("plan 结果见下"); plan = null }) { Text("刷新") }
            Text(
                "模块目录做了隐蔽重定向，路径不再暴露在常规 ls 里。",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
        }

        SectionCard("说明") {
            Text(
                "BOSS 只做 Magic Mount：把模块的 system/ 递归合并进真实 /system，" +
                "分区本身一个字节都不改。这也是首页那条自检能成立的根据。",
                style = MaterialTheme.typography.bodyMedium)
        }
    }
}

@Composable
private fun ModuleRow(m: ModuleInfo) {
    Column(Modifier.fillMaxWidth().padding(vertical = 6.dp)) {
        Text(m.name.ifBlank { m.id }, style = MaterialTheme.typography.bodyLarge)
        Spacer(Modifier.height(2.dp))
        KV("id", m.id)
        KV("版本", m.version.ifBlank { "-" })
        KV("挂载", when {
            !m.enabled -> "已禁用"
            m.scriptOnly -> "仅脚本"
            else -> "是"
        }, warn = !m.enabled)
        if (m.author.isNotBlank()) KV("作者", m.author)
        if (m.description.isNotBlank()) KV("说明", m.description)
    }
    HorizontalDivider(Modifier.padding(top = 6.dp))
}
