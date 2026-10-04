package com.boss.manager.ui.screens

import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.boss.manager.data.BossStatus
import com.boss.manager.data.VerifyReport
import com.boss.manager.ui.BossViewModel
import com.boss.manager.core.RamdiskProbe
import com.boss.manager.ui.KV
import com.boss.manager.ui.SectionCard

/**
 * 首页 = 自检面板。
 *
 * 排序是有讲究的：**劫持**排在最上面且失败了要显眼。
 * 任务5 交接文档约束①说得很清楚——劫持没成的话，模块挂载、systemless 清单、
 * 脚本、隐藏在 2SI 设备上一次都不会执行。用户看到的症状是"什么都没生效"，
 * 如果首页不把这一条顶出来，所有人都会去查错误的地方。
 */
@Composable
fun HomeScreen(vm: BossViewModel, status: BossStatus?) {
    val verify by vm.verify.collectAsStateWithLifecycle()

    Column(Modifier.fillMaxSize().padding(vertical = 8.dp)) {

        if (status != null && !status.hijacked) {
            Card(
                Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 6.dp),
                colors = CardDefaults.cardColors(
                    containerColor = MaterialTheme.colorScheme.errorContainer),
            ) {
                Column(Modifier.padding(16.dp)) {
                    Text("init 接管未生效", fontWeight = FontWeight.Bold,
                        color = MaterialTheme.colorScheme.onErrorContainer)
                    Spacer(Modifier.height(6.dp))
                    Text(
                        "/proc/1/exe 不指向 BOSS。在 Android 10+ 的两段式 init 设备上，" +
                        "模块挂载 / 无修改清单 / 脚本 / 隐藏一次都不会执行——" +
                        "后面看到的「什么都没生效」几乎都是这一条引起的，先修它。",
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onErrorContainer)
                }
            }
        }

        SectionCard("BOSS") {
            if (status == null) {
                Text("读取中…", style = MaterialTheme.typography.bodyMedium)
            } else {
                KV("bossd", if (status.installed) "运行中" else "未运行", warn = !status.installed)
                KV("init 接管", if (status.hijacked) "已生效" else "未生效", warn = !status.hijacked)
                KV("版本", status.version)
                KV("协议", "v${status.protoVersion}")
                KV("SELinux", status.selinux)
                KV("manager uid", status.managerUid?.toString() ?: "未注册")
                KV("模块", "${status.moduleCount} 个")
                KV("BOSS 挂载", "${status.bossMounts} 条")
                // 用户明确要求：有 ramdisk 显示"是"，没有显示"否"。
                // 类型与路径附在后面——模拟器/虚拟机常暴露裸 ramdisk，
                // 那和真机 boot 镜像的处理路径不同，得让人看见是哪一种。
                val rd = status.ramdisk
                KV(
                    "ramdisk",
                    if (rd != null) "是（${rd.kind.label}）" else "否",
                    warn = rd == null,
                )
                rd?.let {
                    KV("  └ 位置", it.path)
                    KV(
                        "  └ 大小",
                        if (it.sizeBytes > 0) "${it.sizeBytes / 1024} KB"
                        else "读不到大小（块设备常见，不影响判定）",
                    )
                } ?: run {
                    // 找不到时把扫过的地方列出来。
                    // 不然用户只能报"显示否"，而"扫过哪些"才是定位的关键信息。
                    val tried = runCatching { RamdiskProbe.searchedPaths() }.getOrNull()
                    if (!tried.isNullOrEmpty()) {
                        Text(
                            "扫过 ${tried.size} 个位置（节选）：",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                        tried.take(4).forEach {
                            Text(
                                "  · $it",
                                style = MaterialTheme.typography.bodySmall.copy(
                                    fontFamily = FontFamily.Monospace
                                ),
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                        }
                    }
                }
            }
        }

        SectionCard("无修改自检（systemless verify）") {
            if (verify == null) Text("读取中…") else VerifyBody(verify!!)
            Spacer(Modifier.height(8.dp))
            Row {
                TextButton(onClick = { vm.refresh() }) { Text("重新自检") }
                Spacer(Modifier.width(8.dp))
                TextButton(onClick = { vm.saveBaseline() }) { Text("存为基线") }
            }
            Text(
                "基线要在**刷机后、装模块前**存一次，之后才有比对对象。",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

@Composable
private fun VerifyBody(v: VerifyReport) {
    KV("① BOSS 挂载", "${v.bossMounts} 条")
    if (v.outOfRange.isNotEmpty()) {
        v.outOfRange.forEach { KV("   越界", it, warn = true) }
    }
    if (v.readOnlyParts.isEmpty()) {
        KV("② 只读分区", "本机没有这些分区（跳过）")
    } else {
        v.readOnlyParts.forEach { (part, ro) ->
            KV("② $part", if (ro) "ro（符合预期）" else "rw（越界）", warn = !ro)
        }
    }
    KV("③ 基线比对", when (v.baselineDiff) {
        null -> "无基线"
        0 -> "与基线一致"
        else -> "有 ${v.baselineDiff} 处差异"
    })
    KV("④ root 痕迹路径",
        if (v.tracePaths.isEmpty()) "无" else v.tracePaths.joinToString("、"))
    KV("结论", v.verdictLabel, warn = v.outOfRange.isNotEmpty())
}
