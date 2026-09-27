package com.boss.manager.ui.screens

import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.boss.manager.ui.BossViewModel
import com.boss.manager.ui.KV
import com.boss.manager.ui.SectionCard

/**
 * 隐藏（特典）页。
 *
 * 这一页最重要的不是功能，是**把边界说清楚**：
 * Android 的 app 进程与 zygote 共享 mount namespace，无注入时"给某个 app
 * 单独摘挂载"做不到——命中名单就摘，共享同一 ns 的进程会一起生效。
 *
 * 交接文档的原话：用错方向的"隐藏"比不隐藏更危险，它给人虚假的安全感。
 * 所以这里不给"一键隐身"开关，而是把影响范围写在用户眼前。
 */
@Composable
fun HideScreen(vm: BossViewModel) {
    val deny by vm.deny.collectAsStateWithLifecycle()
    val exposures by vm.exposures.collectAsStateWithLifecycle()
    var input by remember { mutableStateOf("") }

    Column(Modifier.fillMaxSize().padding(vertical = 8.dp)) {

        Card(
            Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 6.dp),
            colors = CardDefaults.cardColors(
                containerColor = MaterialTheme.colorScheme.secondaryContainer),
        ) {
            Column(Modifier.padding(16.dp)) {
                Text("影响范围（请读完再用）", style = MaterialTheme.typography.titleSmall)
                Spacer(Modifier.height(6.dp))
                Text(
                    "app 进程与 zygote 共享挂载命名空间。命中名单即摘，" +
                    "共享同一命名空间的其它进程也会一起看不到这些挂载——" +
                    "这是当前无注入实现的已知边界，不是故障。真正的按进程隔离" +
                    "要走 zygote 注入，属于后续任务。",
                    style = MaterialTheme.typography.bodyMedium)
            }
        }

        SectionCard("隐藏名单（${deny.size}）") {
            OutlinedTextField(
                value = input,
                onValueChange = { input = it },
                label = { Text("包名或进程名") },
                modifier = Modifier.fillMaxWidth(),
                singleLine = true,
            )
            Spacer(Modifier.height(8.dp))
            Button(
                onClick = {
                    val p = input.trim()
                    if (p.isNotBlank()) { vm.denyAdd(p); input = "" }
                },
                enabled = input.isNotBlank(),
            ) { Text("加入名单") }

            Spacer(Modifier.height(8.dp))
            if (deny.isEmpty()) {
                Text("（空）。空名单时守护进程不会常驻——空转的扫描本身也是痕迹。",
                    style = MaterialTheme.typography.bodyMedium)
            } else {
                deny.forEach { pkg ->
                    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                        Text(pkg, Modifier.weight(1f), style = MaterialTheme.typography.bodyMedium)
                        TextButton(onClick = { vm.denyDel(pkg) }) { Text("移除") }
                    }
                }
            }
        }

        SectionCard("暴露面（BOSS 引入的挂载 ${exposures.size} 条）") {
            if (exposures.isEmpty()) {
                Text("（无）", style = MaterialTheme.typography.bodyMedium)
            } else {
                exposures.forEach { e ->
                    KV(e.type, "${e.src} → ${e.target}")
                }
            }
            Spacer(Modifier.height(6.dp))
            Text(
                "tmpfs 覆盖层也在其中：它的挂载源字面值是 tmpfs，" +
                "按源判断会一条都认不出来，而它恰恰是最该摘掉的一类。",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
        }

        SectionCard("属性伪装") {
            Text(
                "属性伪装是检测面里的一层，不是全部；它不承诺、也不以保证绕过" +
                "任何第三方完整性或风控判定为目标。动态属性（sys.usb.state 之类）" +
                "会被系统服务随时改回，写了也会漂。",
                style = MaterialTheme.typography.bodyMedium)
            Spacer(Modifier.height(8.dp))
            TextButton(onClick = { vm.toast("模板已写入 props.conf，边界说明在文件头") }) {
                Text("生成 props.conf 模板")
            }
        }
    }
}
