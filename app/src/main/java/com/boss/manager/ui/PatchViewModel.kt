package com.boss.manager.ui

import android.app.Application
import android.net.Uri
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.boss.manager.data.PatchRepository
import com.boss.manager.data.fields
import dev.veritpath.Veritpath
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import java.io.File

/**
 * 「修补」页的状态。
 *
 * 刻意**不**并进 BossViewModel：那边管的是"BOSS 装没装、跑得好不好"，
 * 这一页管的是"还没装/OTA 掉了，把镜像修出来"——
 * 两者生命周期不同，而且这一页需要 Context（SAF 拷贝要用缓存目录），
 * 所以单独做成 AndroidViewModel。
 */
class PatchViewModel(app: Application) : AndroidViewModel(app) {

    private val repo = PatchRepository(app)

    private val _version = MutableStateFlow<String?>(null)
    val version: StateFlow<String?> = _version.asStateFlow()

    private val _imagePath = MutableStateFlow<String?>(null)
    val imagePath: StateFlow<String?> = _imagePath.asStateFlow()

    private val _payloadDir = MutableStateFlow<String?>(null)
    val payloadDir: StateFlow<String?> = _payloadDir.asStateFlow()

    /** 分析结果里给 UI 看的几行。 */
    private val _analyze = MutableStateFlow<List<Pair<String, String>>>(emptyList())
    val analyze: StateFlow<List<Pair<String, String>>> = _analyze.asStateFlow()

    /**
     * 用户选定的分区类型。**默认 init_boot**（Android 13+ GKI 主流布局）。
     *
     * 为什么让用户选而不是自动识别：CLI 的 TARGET 字段只是回显传入的 role，
     * 不是真正的分区判别（实测：同一份镜像传 --boot 就报 TARGET:boot）。
     * 唯一可靠的信息是 TARGET:none（没 ramdisk）。所以只能由用户指定。
     */
    private val _role = MutableStateFlow("init_boot")
    val role: StateFlow<String> = _role.asStateFlow()

    fun setRole(r: String) {
        _role.value = r
        _analyze.value = emptyList()
    }

    private val _output = MutableStateFlow<String?>(null)
    val output: StateFlow<String?> = _output.asStateFlow()

    private val _patched = MutableStateFlow<List<File>>(emptyList())
    val patched: StateFlow<List<File>> = _patched.asStateFlow()

    private val _busy = MutableStateFlow(false)
    val busy: StateFlow<Boolean> = _busy.asStateFlow()

    private val _toast = MutableStateFlow<String?>(null)
    val toast: StateFlow<String?> = _toast.asStateFlow()

    init {
        // 一进来就探一次 .so 在不在。
        // System.loadLibrary 失败会抛 UnsatisfiedLinkError，
        // 而它只在**第一次调用时**才抛——提前探一次，就能把
        // "库没打进 APK" 和 "分析失败" 分清楚。
        viewModelScope.launch {
            runCatching { _version.value = repo.version() }
                .onFailure { _toast.value = "veritpath 库加载失败：${it.message}" }
        }
    }

    private fun work(block: suspend () -> Unit) {
        viewModelScope.launch {
            _busy.value = true
            try {
                block()
            } catch (t: Throwable) {
                // UnsatisfiedLinkError 是 Error 不是 Exception，
                // catch (Throwable) 才接得住——只接 Exception 的话
                // 这类问题会直接把 App 崩掉。
                _toast.value = "出错：${t.message}"
            } finally {
                _busy.value = false
            }
        }
    }

    fun toast(msg: String) { _toast.value = msg }
    fun clearToast() { _toast.value = null }

    /** 用户选了镜像文件（SAF Uri）→ 拷进缓存，拿到真实路径。 */
    fun pickImage(uri: Uri) = work {
        val name = repo.guessName(uri, "boot.img")
        _imagePath.value = repo.copyToCache(uri, name)
        _toast.value = "已读入：$name"
    }

    fun clearImage() { _imagePath.value = null; _analyze.value = emptyList() }

    /** 用户选了 payload 目录（SAF 目录 Uri）。 */
    fun pickPayload(uri: Uri) {
        // 目录 Uri 不能直接当路径用——同样要拷。
        // DocumentFile 遍历比较麻烦，这里用一个更稳的做法：
        // 把整个目录拷成缓存下的 payload/，再拿它的路径。
        work {
            val dir = repo.copyTreeToCache(uri)
            _payloadDir.value = dir
            val r = repo.payloadCheck(dir)
            _output.value = r.output
            _toast.value = if (r.ok()) "payload 校验通过" else "payload 校验没过，看下面输出"
        }
    }

    fun analyze() = work {
        val p = _imagePath.value ?: run { _toast.value = "先选一个镜像"; return@work }
        val r = repo.probe(p, _role.value)
        _output.value = r.output
        _analyze.value = r.fields("ARCH", "TARGET", "LAYOUT", "ANDROID", "GKI", "PATCHED")
        val target = r.line("TARGET")?.trim()
        _toast.value = when {
            !r.ok() -> "分析失败（退出码 ${r.exitCode}）—— 看下面输出"
            target == null || target == "none" ->
                "这份镜像里没找到 ramdisk（TARGET:none）——换「boot」再试，或确认选对了文件"
            else -> "分析完成（$target）"
        }
    }

    fun verify(imagePath: String) = work {
        val r = repo.verify(imagePath)
        _output.value = r.output
        _toast.value = if (r.ok()) "校验通过，可以刷" else "校验没过——别刷这个镜像"
    }

    /**
     * 修补。产出落在 app 私有缓存里，之后由用户通过"另存为"导出。
     *
     * ⚠️ 这一步**不能**直接写 sdcard：一是要权限（BOSS 零权限是硬要求），
     * 二是 Android 10+ 的分区存储写不了。用 SAF 的"另存为"交给用户定位置。
     */
    fun patch() = work {
        val img = _imagePath.value ?: run { _toast.value = "先选一个镜像"; return@work }
        val pay = _payloadDir.value ?: run { _toast.value = "先选 payload 目录"; return@work }
        val out = repo.outputDir()
        // inject 的镜像**必须**带 role flag：裸位置参数会被 CLI 完全忽略，
        // 表现为退出码 1（旧版连原因都看不到，因为错误走 stderr 未被捕获）。
        val r = repo.inject(img, pay, out, _role.value)
        _output.value = r.output
        _patched.value = repo.outputs()
        _toast.value = if (r.ok()) "修补完成，导出后用 fastboot 刷入"
                       else "修补失败（退出码 ${r.exitCode}）"
    }

    fun refreshOutputs() = work { _patched.value = repo.outputs() }

    fun clearAll() = work {
        repo.clearCache()
        _imagePath.value = null
        _payloadDir.value = null
        _analyze.value = emptyList()
        _output.value = null
        _patched.value = emptyList()
        _toast.value = "已清空（缓存里的镜像也删了）"
    }
}
