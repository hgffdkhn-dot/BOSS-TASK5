# BOSS · 给任务6（客户端）的交接须知

> 这份文档是给你省时间的。它只写**交接时仍然成立**的东西：哪些是真的验过、
> 哪些只是"代码写完了"、哪些地方看着可疑但其实别去动。
>
> 设计推演看 `docs/TASK5-无修改系统逻辑与特典逻辑.md`，这份是操作手册。

---

## 1. 先记住三条硬约束

这三条在交接的那一刻**仍然成立**。不看它们，你会把时间花在错的地方。

### ① v0.2 的 init 接管：代码已补齐，但**只能真机验**

沙盒没有内核，也没有 SwitchRoot 可观察。我最多验到"命令拼对了、
dry run 没有副作用、变砖保护还在"，**验不到"切根后真的落到 `/system/bin/init`"**。

**所以你的第一件事不是写代码，是确认它有没有成：**

```bash
ls -l /proc/1/exe        # 指向 boss = 劫持成功；指向原厂 init = 没成
```

这比翻日志快得多——那个阶段日志可能一个字节都没有。
**上机务必带 `boss_selinux=0` 自救路径**（cmdline 加这一项就能退回原厂流程，不用重刷包）。

这一条没过，后面所有功能（模块挂载、systemless 清单、脚本、隐藏）
在 2SI 设备上**一次都不会执行**。症状是"代码写完、单测全绿、真机无效果"，
排查方向极易跑偏到"我的挂载逻辑写错了"——不是，是没人拉它起来。

### ② 改测试：root 与非 root **两种身份都要跑一遍**

CI 的 runner 是**非 root**。只跑 root 会得出完全错误的结论——
这条我在交接前刚用一次全红买过：`init` 标了 `needs_root=1`，
非 root 下分发器在调用目标函数**之前**就返回 1，
于是参数转发、变砖保护、dry run 一个都没跑到。本地 root 全绿、CI 全红。

```bash
bash tools/xxx_test.sh                                        # root
setpriv --reuid=65534 --regid=65534 --clear-groups bash ...   # 非 root
```

**以非 root 那一份为准。** 同理，新增任何组件时想清楚：
它真的需要 root 吗？标了 `needs_root` 就等于在 CI 上验不到它（接力须知坑 7）。

### ③ 仓库可能还没推上去

任务5 交付时网络出口对 GitHub 写入受限，本地仓库已 init / commit / tag（`v0.2.0`），
但建空仓 + push 这两步得主办方执行（`bash PUSH-TO-GITHUB.sh`）。
**如果你拿不到仓库，先确认这一步有没有做。**

---

## 2. 交到你手上的是什么

| 部分 | 状态 |
|---|---|
| su 子系统 + 守护进程 | ✅ 任务2 已交付 |
| 关键组件（resetprop / 模块挂载 / 脚本 / 工具集 / 开机编排） | ✅ 任务3 已交付 |
| SELinux（255 条规则 + 引擎 + 早期注入接线） | ✅ 任务4 已交付，**未上真机** |
| 无修改系统逻辑（systemless） | ✅ 任务5 已交付，自检可跑 |
| 特典（隐藏 / 暴露面 / 属性伪装） | ✅ 任务5 已交付，**实际范围需真机确认** |
| v0.2 init 接管（hijack-prep + /init.real + 触发者） | ⚠️ **代码已补齐，真机待验** ← 约束① |
| **BOSS 客户端（App）** | ⬜ **你的活** |

沙盒已验：7 套共 138 项（非 root 与 root 各跑一遍）。
**没验过的**看 `docs/TASK5-...md` 第 8 节——那里逐条列了，别把"没验过"当"验过"。

---

## 3. 你的接口：文件即契约

不需要新协议。`/data/adb/boss/` 下这几个文件就是 App 的读写面：

```
/data/adb/boss/systemless.conf        声明式"系统改动"清单（属性/删除/persist/隐藏项）
/data/adb/boss/systemless.baseline    只读分区基线快照（刷机后先存一份）
/data/adb/boss/denylist.conf          隐藏名单（一行一个包名/进程名）
/data/adb/boss/props.conf             属性伪装清单
/data/adb/boss/modules/<id>/          模块（Magisk 格式，目录做了隐蔽重定向）
/data/adb/boss/boss.log               审计日志
```

**CLI**（返回码统一，沿用任务4 的语义）：

| 命令 | 用途 | 返回码 |
|---|---|---|
| `boss systemless apply\|plan\|verify\|status` | 无修改系统逻辑 | 0 全应用 / 3 部分应用 / 1 失败 / 2 无能力 |
| `boss hide denylist add\|del\|list` | 隐藏名单 | 0 / 1 |
| `boss hide mounts` | 暴露面清单 | 0 / 1 |
| `boss hide umount <pid>` | 摘挂载（需 root） | 0 / 1；3 = 进不去目标 ns |
| `boss hide daemon` | 常驻扫描 | — |
| `boss hide props` | 属性伪装模板 | 同上 systemless |

**`3` 是部分应用，不是错误。** 它让一条规则在某机型上失效时不至于中断开机。
UI 上别把它显示成失败。

**改协议时**：扩展 `struct boss_request` **必须同时升 `BOSS_PROTO_VER`**，
否则新旧版本二进制静默错位。这条从任务2 交接时就在，仍然有效。

**授权决策**改 `policy_decide()`，保持 allow / deny / prompt 三元语义（接力须知第 7 节）。

---

## 4. 三条别做错的事

### ① 别把隐藏做成"一键隐身"按钮

**Android 的 app 进程与 zygote 共享 mount namespace。**
无注入时"给某个 app 单独摘挂载"做不到——命中名单即摘，
**共享同一 ns 的进程会一起生效**。

真按进程隔离要走 zygisk 式注入（app 进程内 `unshare(CLONE_NEWNS)` 后再 umount），
属后续任务。

所以 UI 上应当说清影响范围，而不是给一个看着万能的开关。
**用错方向的"隐藏"比不隐藏更危险**：它给人虚假的安全感。

### ② 别把属性伪装当万灵药

它是检测面里的**一层**，不是全部，**不承诺、也不以保证绕过任何第三方
完整性/风控判定为目标**。动态属性（`sys.usb.state` 之类）会被系统服务随时改回，
写了也会漂。BOSS 主打隐蔽是产品定位，但隐蔽不等于欺骗用户。

生成的 `props.conf` 模板第一行就写着这个边界，别在 App 里把它删掉。

### ③ 别为了跑测试去拆 `needs_root`

`init` 标了 `needs_root=1` 是**正确语义**（真机上由 rc 以 root 调起，
非 root 明确报错、不静默失败）。它挡住了 CI 上的测试，
但解法是让测试绕过去（`tests/initkit.c` 直调 `boss_init_main`），
不是把产品的 root 语义改松。

---

## 5. 上手路径

```bash
# 0) 先确认仓库到位（约束③）
git log --oneline -1 && git tag

# 1) 真机第一件事（约束①）
ls -l /proc/1/exe

# 2) 本地两种身份各跑一遍（约束②）
make test && make initkit
bash tools/systemless_test.sh
bash tools/hide_test.sh
bash tools/hijack_test.sh
setpriv --reuid=65534 --regid=65534 --clear-groups bash tools/hijack_test.sh

# 3) 你要接的东西
./build/boss systemless status
./build/boss hide mounts
./build/boss hide denylist list
boss ping                       # 探活（已可用）
```

---

## 6. 真机验收清单

抄 `docs/TASK5-无修改系统逻辑与特典逻辑.md` 第 7 节，这里只留最要命的三条：

```
[ ] ls -l /proc/1/exe 指向 boss（其余全部依赖它）
[ ] 加 boss_selinux=0 后仍能正常开机（自救路径可用）
[ ] 刷回原厂镜像仍能开机（这条没了，迭代就停了）
```

---

## 7. 已知风险（一句话版）

| 项 | 状态 |
|---|---|
| 劫持的实际效果 | **未上真机**（约束①） |
| 隐藏的实际范围 | **未上真机**（约束①） |
| SELinux 255 条规则 | **未上真机**，靠 `permissive boss` 兜底 |
| post-fs-data 触发者的时序 | 轮询触发，比 init 原生 trigger 晚几十 ms~几秒 |
| 带内置 sepol 的产物体积 | +224KB，上真机前确认分区放得下 |
| hexpatch 退路 | 大概率永不触发（`/init` 正在执行时 open 会 ETXTBSY） |

完整版见 `docs/TASK5-...md` 第 8 节。

---

## 8. 别浪费时间的地方

- **别给 su 客户端加特权**（架构红线第 1 条）。
- **别做 macOS / Windows 支持**（IPC 依赖 Linux 抽象命名空间 + `SO_PEERCRED`）。
- **别在注入阶段造 `/su`、`/system/bin/su`**（兼容性留给 App 按需挂载）。
- **别用 sqlite 重写策略**（等 App 稳定了再换存储，调用方一行不用改）。
- **别往 `/system` 落任何东西**（这是 BOSS 与 Magisk 定位差异的根本）。
- **别信"编译通过就行"**（PT_TLS 对齐、非 PIE 这两个门槛编译期完全不报）。
