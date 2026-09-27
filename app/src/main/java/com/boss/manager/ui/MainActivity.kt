package com.boss.manager.ui

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.viewModels
import androidx.compose.foundation.layout.*
/* ⚠️ Icons 不在 material3 包里。Icons 属于 androidx.compose.material.icons，
 * 而 `import androidx.compose.material3.*` 并不会带进来——
 * 漏了它的表现是 Unresolved reference: Icons，而且只在这一处。
 * 顺带一提：Extension / Security / VisibilityOff 属于 icons-extended（不是 core），
 * 所以 build.gradle.kts 里必须挂 material-icons-extended 而不是 core。 */
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Extension
import androidx.compose.material.icons.filled.Home
import androidx.compose.material.icons.filled.Security
import androidx.compose.material.icons.filled.VisibilityOff
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.unit.dp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import com.boss.manager.ui.screens.*
import com.boss.manager.ui.theme.BossTheme
import kotlinx.coroutines.launch

class MainActivity : ComponentActivity() {

    private val vm: BossViewModel by viewModels()

    override fun onCreate(savedInstanceState: Bundle?) {
        // Android 16（API 36）起 edge-to-edge 是强制的，旧的 opt-out 会被忽略。
        // 主动声明它，让内容自己处理 insets，而不是等系统把状态栏压上来。
        enableEdgeToEdge()
        super.onCreate(savedInstanceState)

        setContent {
            BossTheme {
                val status by vm.status.collectAsStateWithLifecycle()
                val busy by vm.busy.collectAsStateWithLifecycle()
                val toast by vm.toast.collectAsStateWithLifecycle()

                var tab by remember { mutableIntStateOf(0) }

                // 回到前台就刷一次：挂载状态、模块、暴露面都是会变的，
                // 缓存着显示会给出过期的"干净"。
                LaunchedEffect(Unit) {
                    vm.refresh()
                    lifecycleScope.launch {
                        repeatOnLifecycle(Lifecycle.State.RESUMED) {
                            vm.startPromptPolling()
                        }
                    }
                }
                DisposableEffect(Unit) { onDispose { vm.stopPromptPolling() } }

                val snack = remember { SnackbarHostState() }
                LaunchedEffect(toast) {
                    toast?.let { snack.showSnackbar(it); vm.clearToast() }
                }

                Scaffold(
                    snackbarHost = { SnackbarHost(snack) },
                    bottomBar = { BossNav(tab) { tab = it } },
                ) { pad ->
                    Column(Modifier.padding(pad)) {
                        // Expressive 的加载指示器：波形进度条，取代旧的直线进度条
                        if (busy) {
                            LoadingIndicator(Modifier.padding(vertical = 8.dp).fillMaxWidth())
                        }
                        when (tab) {
                            0 -> HomeScreen(vm, status)
                            1 -> ModuleScreen(vm)
                            2 -> HideScreen(vm)
                            3 -> SuperuserScreen(vm)
                        }
                    }
                }
            }
        }
    }
}

private enum class Tab(val label: String, val icon: ImageVector) {
    HOME("状态", Icons.Default.Home),
    MODULE("模块", Icons.Default.Extension),
    HIDE("隐藏", Icons.Default.VisibilityOff),
    SU("授权", Icons.Default.Security),
}

@Composable
private fun BossNav(current: Int, onSelect: (Int) -> Unit) {
    NavigationBar {
        Tab.entries.forEachIndexed { i, t ->
            NavigationBarItem(
                selected = i == current,
                onClick = { onSelect(i) },
                icon = { Icon(t.icon, contentDescription = t.label) },
                label = { Text(t.label) },
            )
        }
    }
}

/**
 * 卡片容器。圆角用 extraLargeIncreased——Expressive 新增的那一档，
 * 大容器用它才有 Android 16 那代的观感。
 */
@Composable
fun SectionCard(title: String, content: @Composable ColumnScope.() -> Unit) {
    ElevatedCard(
        modifier = Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 6.dp),
        shape = MaterialTheme.shapes.extraLargeIncreased,
    ) {
        Column(Modifier.padding(16.dp)) {
            Text(title, style = MaterialTheme.typography.titleMedium)
            Spacer(Modifier.height(8.dp))
            content()
        }
    }
}

@Composable
fun KV(k: String, v: String, warn: Boolean = false) {
    Row(Modifier.fillMaxWidth().padding(vertical = 2.dp)) {
        Text(k, Modifier.weight(1f), style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant)
        Text(v, style = MaterialTheme.typography.bodyMedium,
            color = if (warn) MaterialTheme.colorScheme.error
                    else MaterialTheme.colorScheme.onSurface)
    }
}
