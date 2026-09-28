package com.boss.manager.ui.screens

import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.compose.ui.unit.dp
import com.boss.manager.ui.PatchViewModel
import com.boss.manager.ui.KV
import com.boss.manager.ui.SectionCard
import java.io.File

/**
 * 「修补」页——装机前 / OTA 掉了之后，把 boot|init_boot 镜像修出来。
 *
 * ⚠️ 这一页**不做刷入**。刷镜像必须走 fastboot，App 在 Android 上做不到
 * （没有访问 bootloader 的能力，也不该有）。所以流程终点是"导出修补后的镜像"，
 * 由用户拿到电脑上 fastboot flash。
 *
 * 这个边界要在界面上说清楚，否则用户会以为点了就能装好——
 * 那种误解比没有这个功能更糟。
 */
@Composable
fun PatchScreen(vm: PatchViewModel) {
    val context = LocalContext.current
    val busy by vm.busy.collectAsStateWithLifecycle()
    val version by vm.version.collectAsStateWithLifecycle()
    val imagePath by vm.imagePath.collectAsStateWithLifecycle()
    val payloadDir by vm.payloadDir.collectAsStateWithLifecycle()
    val analyze by vm.analyze.collectAsStateWithLifecycle()
    val output by vm.output.collectAsStateWithLifecycle()
    val patched by vm.patched.collectAsStateWithLifecycle()
    val role by vm.role.collectAsStateWithLifecycle()

    // ---- SAF：系统文件选择器。选到的是 Uri，不是路径。
    //      路径转换在 PatchRepository 里做（拷进私有缓存）。
    val pickImage = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri: Uri? -> uri?.let { vm.pickImage(it) } }

    val pickPayload = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocumentTree()
    ) { uri: Uri? -> uri?.let { vm.pickPayload(it) } }

    var exportFile by remember { mutableStateOf<File?>(null) }
    val saveOut = rememberLauncherForActivityResult(
        ActivityResultContracts.CreateDocument("application/octet-stream")
    ) { uri: Uri? ->
        val f = exportFile
        if (uri != null && f != null) {
            runCatching {
                context.contentResolver.openOutputStream(uri)?.use { o ->
                    f.inputStream().use { i -> i.copyTo(o) }
                }
            }.onSuccess { vm.toast("已导出：${f.name}") }
                .onFailure { vm.toast("导出失败：${it.message}") }
        }
        exportFile = null
    }

    Column(
        Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(bottom = 16.dp)
    ) {
        SectionCard("镜像修补（veritpath ${version ?: "加载中…"}）") {
            Text(
                "选镜像 → 选 payload → 修补 → 导出。\n" +
                    "导出后的镜像仍需 fastboot flash 刷入，App 无法代劳。",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }

        SectionCard("1 · 镜像") {
            // 分区类型必须由用户选。CLI 的 TARGET 只是回显传入的 role，
            // 不是真正的分区判别，所以自动识别做不到——
            // 而选错的后果是产出一个要刷到错误分区的镜像。
            Text("分区类型", style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
            Spacer(Modifier.height(4.dp))
            Row(Modifier.fillMaxWidth()) {
                listOf("init_boot", "boot", "vendor_boot").forEach { r ->
                    FilterChip(
                        selected = role == r,
                        onClick = { vm.setRole(r) },
                        label = { Text(r) },
                        modifier = Modifier.padding(end = 6.dp),
                    )
                }
            }
            Spacer(Modifier.height(8.dp))
            Button(
                onClick = { pickImage.launch(arrayOf("*/*")) },
                enabled = !busy,
                modifier = Modifier.fillMaxWidth(),
            ) { Text("选择 boot / init_boot 镜像") }

            imagePath?.let {
                Spacer(Modifier.height(8.dp))
                KV("已读入", File(it).name)
                Spacer(Modifier.height(8.dp))
                Row(Modifier.fillMaxWidth()) {
                    Button(
                        onClick = { vm.analyze() },
                        enabled = !busy,
                        modifier = Modifier.weight(1f),
                    ) { Text("分析") }
                    Spacer(Modifier.width(8.dp))
                    OutlinedButton(
                        onClick = { vm.clearImage() },
                        modifier = Modifier.weight(1f),
                    ) { Text("清除") }
                }
            }

            if (analyze.isNotEmpty()) {
                Spacer(Modifier.height(8.dp))
                analyze.forEach { (k, v) -> KV(k, v) }
            }
        }

        SectionCard("2 · payload") {
            Button(
                onClick = { pickPayload.launch(null) },
                enabled = !busy,
                modifier = Modifier.fillMaxWidth(),
            ) { Text("选择 payload 目录") }
            payloadDir?.let {
                Spacer(Modifier.height(8.dp))
                KV("已读入", File(it).name)
            }
            Text(
                "payload 至少要有 manifest.json、init.boss.rc 和对应架构的 boss。",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }

        SectionCard("3 · 修补") {
            Button(
                onClick = { vm.patch() },
                enabled = !busy && imagePath != null && payloadDir != null,
                modifier = Modifier.fillMaxWidth(),
            ) { Text("修补镜像") }

            if (patched.isNotEmpty()) {
                Spacer(Modifier.height(8.dp))
                patched.forEach { f ->
                    Row(Modifier.fillMaxWidth().padding(vertical = 4.dp)) {
                        Text(
                            "${f.name}（${f.length() / 1024} KB）",
                            Modifier.weight(1f),
                            style = MaterialTheme.typography.bodyMedium,
                        )
                        TextButton(onClick = { vm.verify(f.absolutePath) }) { Text("校验") }
                        TextButton(onClick = {
                            exportFile = f
                            saveOut.launch(f.name)
                        }) { Text("导出") }
                    }
                }
                Spacer(Modifier.height(8.dp))
                TextButton(onClick = { vm.clearAll() }) { Text("清空缓存") }
            }
        }

        output?.let { text ->
            SectionCard("输出") {
                Text(
                    text,
                    style = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace),
                    modifier = Modifier
                        .fillMaxWidth()
                        .heightIn(max = 240.dp)
                        .verticalScroll(rememberScrollState())
                        .horizontalScroll(rememberScrollState()),
                )
            }
        }
    }
}
