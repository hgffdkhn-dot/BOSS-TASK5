package com.boss.manager.ui

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.boss.manager.core.BossIpc
import com.boss.manager.data.*
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

/**
 * 一个 ViewModel 管住全部页面状态。
 *
 * 刻意不做"每个页面一个 ViewModel"：BOSS 的状态是同一份事实
 * （装没装、劫持成没成、有几个模块、暴露面几条），拆开之后
 * 必然出现"首页说装好了、模块页说没装"这种只对一半的界面。
 */
class BossViewModel : ViewModel() {

    private val repo = BossRepository()

    private val _status = MutableStateFlow<BossStatus?>(null)
    val status: StateFlow<BossStatus?> = _status.asStateFlow()

    private val _verify = MutableStateFlow<VerifyReport?>(null)
    val verify: StateFlow<VerifyReport?> = _verify.asStateFlow()

    private val _exposures = MutableStateFlow<List<MountExposure>>(emptyList())
    val exposures: StateFlow<List<MountExposure>> = _exposures.asStateFlow()

    private val _deny = MutableStateFlow<List<String>>(emptyList())
    val deny: StateFlow<List<String>> = _deny.asStateFlow()

    private val _modules = MutableStateFlow<List<ModuleInfo>>(emptyList())
    val modules: StateFlow<List<ModuleInfo>> = _modules.asStateFlow()

    private val _policy = MutableStateFlow<PolicySnapshot?>(null)
    val policy: StateFlow<PolicySnapshot?> = _policy.asStateFlow()

    private val _audit = MutableStateFlow<List<AuditEntry>>(emptyList())
    val audit: StateFlow<List<AuditEntry>> = _audit.asStateFlow()

    private val _prompts = MutableStateFlow<List<PromptRequest>>(emptyList())
    val prompts: StateFlow<List<PromptRequest>> = _prompts.asStateFlow()

    private val _busy = MutableStateFlow(false)
    val busy: StateFlow<Boolean> = _busy.asStateFlow()

    private val _toast = MutableStateFlow<String?>(null)
    val toast: StateFlow<String?> = _toast.asStateFlow()

    private var promptJob: Job? = null

    private fun work(block: suspend () -> Unit) {
        viewModelScope.launch {
            _busy.value = true
            try {
                block()
            } catch (t: Throwable) {
                _toast.value = "出错：${t.message}"
            } finally {
                _busy.value = false
            }
        }
    }

    fun refresh() = work {
        _status.value = repo.status()
        _verify.value = repo.verify()
        _exposures.value = repo.exposures()
        _deny.value = repo.denyList()
        _modules.value = repo.modules()
        _policy.value = repo.policy()
        _audit.value = repo.auditLog()
    }

    fun toast(msg: String) { _toast.value = msg }
    fun clearToast() { _toast.value = null }

    fun saveBaseline() = work {
        _verify.value = repo.verify(saveBaseline = true)
        _toast.value = "基线已存：下次 verify 才有比对对象"
    }

    fun denyAdd(pkg: String) = work {
        val r = repo.denyAdd(pkg)
        _toast.value = when {
            r.isDown -> "连不上 bossd"
            r.exitCode == 0 -> "已加入名单（$pkg）"
            else -> "失败（${BossIpc.ExitCode.of(r.exitCode).label}）"
        }
        _deny.value = repo.denyList()
    }

    fun denyDel(pkg: String) = work {
        repo.denyDel(pkg)
        _deny.value = repo.denyList()
    }

    fun policySetDefault(d: String) = work {
        repo.policySetDefault(d)
        _policy.value = repo.policy()
    }

    fun policySetLog(on: Boolean) = work {
        repo.policySetLog(on)
        _policy.value = repo.policy()
    }

    fun refreshAudit() = work { _audit.value = repo.auditLog() }

    /** 弹窗轮询只在页面落到前台时开，后台轮询是纯粹的痕迹与耗电。 */
    fun startPromptPolling() {
        promptJob?.cancel()
        promptJob = viewModelScope.launch {
            while (true) {
                runCatching { _prompts.value = repo.pendingPrompts() }
                delay(3000)
            }
        }
    }

    fun stopPromptPolling() { promptJob?.cancel(); promptJob = null }

    fun answer(req: PromptRequest, allow: Boolean, remember: Boolean) = work {
        repo.answerPrompt(req.id, allow)
        if (remember) repo.rememberApp(req.uid, allow)
        _prompts.value = repo.pendingPrompts()
        _audit.value = repo.auditLog()
    }
}
