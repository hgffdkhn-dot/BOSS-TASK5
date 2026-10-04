package com.boss.manager.ui

import android.app.Application
import android.net.Uri
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.boss.manager.core.RamdiskProbe
import com.boss.manager.core.RootShell
import com.boss.manager.data.LocalInstallRepository
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

/**
 * 本机安装的状态机。
 *
 * **刻意做成一步一步点，不做"一键安装"。**
 * 每步都能看到结果，出问题能停在出问题的那一步——
 * 一键到底的脚本在变砖场景下是最糟的设计：你既不知道断在哪，
 * 也没有机会在写分区之前喊停。
 */
class LocalInstallViewModel(app: Application) : AndroidViewModel(app) {

    private val repo = LocalInstallRepository(app)

    private val _source = MutableStateFlow<RootShell.Source?>(null)
    val source: StateFlow<RootShell.Source?> = _source.asStateFlow()

    private val _part = MutableStateFlow<LocalInstallRepository.PartitionInfo?>(null)
    val part: StateFlow<LocalInstallRepository.PartitionInfo?> = _part.asStateFlow()

    /**
     * 环境里探到的 ramdisk（可能是 boot 镜像，也可能是模拟器暴露的裸 cpio）。
     * **独立于 BOSS 是否已装**——虚拟机上 BOSS 没装，但照样可能有 ramdisk，
     * 那正是"先在虚拟机里试、怕变砖"的用户要看的东西。
     */
    private val _ramdisk = MutableStateFlow<RamdiskProbe.Found?>(null)
    val ramdisk: StateFlow<RamdiskProbe.Found?> = _ramdisk.asStateFlow()

    private val _payloadDir = MutableStateFlow<String?>(null)
    val payloadDir: StateFlow<String?> = _payloadDir.asStateFlow()

    private val _backup = MutableStateFlow<String?>(null)
    val backup: StateFlow<String?> = _backup.asStateFlow()

    private val _patched = MutableStateFlow<String?>(null)
    val patched: StateFlow<String?> = _patched.asStateFlow()

    private val _log = MutableStateFlow<List<String>>(emptyList())
    val log: StateFlow<List<String>> = _log.asStateFlow()

    private val _busy = MutableStateFlow(false)
    val busy: StateFlow<Boolean> = _busy.asStateFlow()

    /** 写回确认开关：只有用户点了确认才为 true，flash 会检查它。 */
    private val _confirmed = MutableStateFlow(false)
    val confirmed: StateFlow<Boolean> = _confirmed.asStateFlow()

    fun setConfirmed(v: Boolean) { _confirmed.value = v }

    private fun say(msg: String) { _log.value = _log.value + msg }

    private fun work(block: suspend () -> Unit) {
        viewModelScope.launch {
            _busy.value = true
            try {
                block()
            } catch (t: Throwable) {
                // UnsatisfiedLinkError 等是 Error 不是 Exception
                say("异常：${t.message}")
            } finally {
                _busy.value = false
            }
        }
    }

    /** 0) 探测 root 与分区。进页面就跑。 */
    fun probe() = work {
        say("== 探测 root ==")
        val src = RootShell.detect()
        _source.value = src
        when (src) {
            is RootShell.Source.Boss -> say("  BOSS daemon 可用（首选通道）")
            is RootShell.Source.SystemSu -> say("  系统 su：${src.path}")
            null -> {
                say("  拿不到 root —— 本机安装需要 root")
                say("  （设备上已有 su，或 BOSS 已装并授权）")
                return@work
            }
        }
        val p = repo.findPartition()
        _part.value = p
        if (p == null) say("  没找到 boot / init_boot 分区 —— 可能跑在模拟器/虚拟机上")
        else say("  分区：${p.dev}（${p.sizeBytes / 1048576}MB）")

        // ramdisk 探测独立于分区：模拟器常常没有 by-name，但会暴露 ramdisk。
        val rd = RamdiskProbe.find()
        _ramdisk.value = rd
        say(
            if (rd != null) "  ramdisk：是（${rd.kind.label}）@ ${rd.path}"
            else "  ramdisk：否（候选位置都没找到）"
        )
        if (rd != null && !rd.kind.isBootImage) {
            say("  ⚠ 这是**裸 ramdisk**，没有 boot 头——")
            say("    veritpath inject 改的是 boot 镜像的 ramdisk 段，对它不适用。")
        }
    }

    fun pickPayload(uri: Uri) = work {
        val d = repo.preparePayload(uri)
        _payloadDir.value = d
        say(if (d != null) "payload 已就绪" else "payload 目录是空的")
    }

    /** 1) 读出分区（同时就是备份） */
    fun dump() = work {
        val p = _part.value ?: run { say("先探测分区"); return@work }
        val s = repo.dump(p)
        say(s.message)
        if (s.ok) {
            _backup.value = s.backup
            say("  备份保留在 ${s.backup}（不会自动删除）")
        }
    }

    /** 2) 修补 */
    fun patch() = work {
        val p = _part.value ?: run { say("先探测分区"); return@work }
        val pay = _payloadDir.value ?: run { say("先选 payload 目录"); return@work }
        // 工作副本必须由 ① 产生——不在这里偷偷补做，
        // 否则用户跳过了①也照样能修补，而那时根本还没有备份。
        val workImg = "/data/local/tmp/boss-local/${p.role}.img"
        if ((repo.sizeOf(workImg) ?: 0L) <= 0L) {
            say("工作副本不存在——请先执行 ① 读出分区")
            return@work
        }
        val s = repo.patch(workImg, pay, p.role)
        say(s.message)
        if (s.ok) {
            _patched.value = s.patched
            val v = repo.verifyLocal(s.patched!!, p.role)
            say(if (v) "  verify 通过" else "  ⚠ verify 没过——不要继续")
        }
    }

    /** 3) 写回前的五道闸 */
    private suspend fun gate(p: LocalInstallRepository.PartitionInfo): Boolean {
        val patched = _patched.value ?: run { say("先修补"); return false }
        val backup = _backup.value ?: run { say("先读出分区（备份）"); return false }
        val g = repo.preflight(p, patched, backup, p.role)
        say("  ${g.message}")
        if (!g.pass) return false
        val v = repo.verifyLocal(patched, p.role)
        say(if (v) "  ④ verify 通过" else "  ④ ⚠ verify 没过")
        return v
    }

    /** 4) 写回。**必须已确认** */
    fun flash() = work {
        val p = _part.value ?: run { say("先探测分区"); return@work }
        if (!_confirmed.value) { say("请先在下面勾选确认"); return@work }
        if (!gate(p)) { say("闸没过，拒绝写回"); return@work }
        val s = repo.flash(p, _patched.value!!, true)
        say(s.message)
        _confirmed.value = false
    }

    /** 回退：还原原厂分区 */
    fun restore() = work {
        val p = _part.value ?: run { say("先探测分区"); return@work }
        val b = _backup.value ?: run { say("没有备份可还原"); return@work }
        say(repo.restore(p, b).message)
    }
}
