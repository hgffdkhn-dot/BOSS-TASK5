package com.boss.manager.ui.screens

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.boss.manager.core.RootShell
import com.boss.manager.ui.KV
import com.boss.manager.ui.LocalInstallViewModel
import com.boss.manager.ui.SectionCard

/**
 * 本机安装：读分区 → 修补 → **dd 写回**，不用电脑。
 *
 * ⚠️ 这一页是全 App 唯一能变砖的地方。
 * 所以界面上的安排都是围绕"让人在动手前看清楚"：
 *   · 顶部红字警告常驻，不折叠
 *   · 分成三步，每步单独点，不做一键到底
 *   · 写回按钮默认禁用，必须先勾确认
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun LocalInstallScreen(vm: LocalInstallViewModel) {
    val source by vm.source.collectAsStateWithLifecycle()
    val ramdisk by vm.ramdisk.collectAsStateWithLifecycle()
    val part by vm.part.collectAsStateWithLifecycle()
    val payloadDir by vm.payloadDir.collectAsStateWithLifecycle()
    val backup by vm.backup.collectAsStateWithLifecycle()
    val patched by vm.patched.collectAsStateWithLifecycle()
    val log by vm.log.collectAsStateWithLifecycle()
    val busy by vm.busy.collectAsStateWithLifecycle()
    val confirmed by vm.confirmed.collectAsStateWithLifecycle()

    LaunchedEffect(Unit) { vm.probe() }

    val pickPayload = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocumentTree()
    ) { uri -> uri?.let { vm.pickPayload(it) } }

    Column(
        Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(bottom = 16.dp)
    ) {
        // ---- 常驻警告 ----
        SectionCard("本机安装（有 su 时）") {
            Text(
                "直接 dd 写回分区，不用电脑。\n" +
                    "但写错分区、写超长、写成空文件，下一次开机就是砖。\n" +
                    "原厂分区会自动备份，且不会删除——救砖靠它。",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.error,
            )
        }

        // ---- 0. 前置 ----
        SectionCard("0 · 前置条件") {
            when (source) {
                is RootShell.Source.Boss -> KV("root 通道", "BOSS daemon（首选）")
                is RootShell.Source.SystemSu ->
                    KV("root 通道", "系统 su ${(source as RootShell.Source.SystemSu).path}")
                null -> KV("root 通道", "未获得 —— 本机安装需要 root")
            }
            part?.let {
                Spacer(Modifier.height(4.dp))
                KV("分区", it.dev)
                KV("大小", "${it.sizeBytes / 1048576} MB")
            } ?: KV("分区", "未探测到 boot / init_boot（模拟器常见）")

            Spacer(Modifier.height(6.dp))
            // ramdisk 与分区分开看：模拟器上常常**没有 by-name 分区，
            // 但有 ramdisk**——这时候"能不能动手"取决于后者。
            val rd = ramdisk
            KV("ramdisk", if (rd != null) "是（${rd.kind.label}）" else "否",
                warn = rd == null)
            rd?.let {
                KV("  └ 位置", it.path)
                KV("  └ 大小", "${it.sizeBytes / 1024} KB")
                if (!it.kind.isBootImage) {
                    Text(
                        "这是裸 ramdisk（无 boot 头）。veritpath inject 改的是 boot " +
                            "镜像的 ramdisk 段，对裸 ramdisk 不适用——此页的①②会失效，勿用。",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.error,
                    )
                }
            }
        }

        // ---- 1. payload ----
        SectionCard("1 · payload") {
            Button(
                onClick = { pickPayload.launch(null) },
                enabled = !busy,
                modifier = Modifier.fillMaxWidth(),
            ) { Text("选择 payload 目录") }
            payloadDir?.let {
                Spacer(Modifier.height(6.dp))
                KV("已就绪", "（已复制到私有缓存）")
            }
            Text(
                "payload 需含 manifest.json、init.boss.rc 与对应架构的 boss。" +
                    "架构不对不会报错，刷进去才是 Exec format error。",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }

        // ---- 2. 分步 ----
        SectionCard("2 · 分步执行（每步都看结果再走下一步）") {
            val ready = source != null && part != null

            Button(
                onClick = { vm.dump() },
                enabled = !busy && ready,
                modifier = Modifier.fillMaxWidth(),
            ) { Text("① 读出分区（＝备份原厂）") }

            Spacer(Modifier.height(8.dp))
            Button(
                onClick = { vm.patch() },
                enabled = !busy && ready && payloadDir != null,
                modifier = Modifier.fillMaxWidth(),
            ) { Text("② 修补 + 自检") }

            backup?.let {
                Spacer(Modifier.height(6.dp))
                KV("备份", it)
            }
            patched?.let {
                Spacer(Modifier.height(2.dp))
                KV("修补产物", it)
            }
        }

        // ---- 3. 写回（危险区）----
        SectionCard("3 · 写回分区（危险）") {
            Row(verticalAlignment = androidx.compose.ui.Alignment.CenterVertically) {
                Checkbox(
                    checked = confirmed,
                    onCheckedChange = { vm.setConfirmed(it) },
                )
                Text(
                    "我已确认：备份存在、产物已自检、分区正确",
                    style = MaterialTheme.typography.bodySmall,
                )
            }
            Spacer(Modifier.height(8.dp))
            Button(
                onClick = { vm.flash() },
                enabled = !busy && confirmed && patched != null && part != null,
                colors = ButtonDefaults.buttonColors(
                    containerColor = MaterialTheme.colorScheme.error,
                    contentColor = MaterialTheme.colorScheme.onError,
                ),
                modifier = Modifier.fillMaxWidth(),
            ) { Text("写回分区（不可撤销）") }

            Spacer(Modifier.height(8.dp))
            OutlinedButton(
                onClick = { vm.restore() },
                enabled = !busy && backup != null && part != null,
                modifier = Modifier.fillMaxWidth(),
            ) { Text("还原原厂分区（救砖）") }
        }

        if (log.isNotEmpty()) {
            SectionCard("日志") {
                Text(
                    log.joinToString("\n"),
                    style = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace),
                    modifier = Modifier.fillMaxWidth(),
                )
            }
        }
    }
}
