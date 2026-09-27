# BOSS · 任务6：BOSS 客户端与全部功能组件对接

> 前置阅读：`docs/HANDOFF-TASK6-客户端.md`（5 号留给我的操作手册）、
> `docs/TASK5-无修改系统逻辑与特典逻辑.md`（设计推演）、
> `docs/HANDOFF-接力须知.md`（红线与已踩的坑）。
>
> 这份写**从别处看不出来**的东西：任务6 到底卡在哪、我怎么解开的、
> 哪些地方看着可疑但其实别去动、哪些我没验过。
>
> 一句话：**任务6 的难点不在"画界面"，在于一个鸡生蛋问题——
> App 必须先被放行，才能写下放行自己的规则。**

---

## 交接时仍然成立的三条硬约束

5 号交接时点名的三条，到我交付时**一条都没消失**。它们的共同点是：
违反了不会立刻报错，而是让你在错误的方向上花掉几天。

### ① v0.2 的 init 接管——代码已补齐，但**只能真机验**

沙盒没有内核、没有 SwitchRoot 可观察。5 号最多验到"命令拼对了、dry run
没有副作用、变砖保护还在"，**验不到"切根后真的落到 `/system/bin/init`"**。

所以接手一台没验过的机器时，**第一件事不是写代码**：

```bash
ls -l /proc/1/exe        # 指向 boss = 劫持成功；指向原厂 init = 没成
```

这一条没过，App 里**每一个功能都会表现为"没生效"**，而排查方向会跑偏到
"我的解析写错了"。不是——是没人拉它起来。
我把这条做成了首页最顶上的告警卡（见「3.4 首页为什么把劫持顶在最上面」）。

**上机务必带 `boss_selinux=0` 自救路径**（cmdline 加一项就能退回原厂流程，不用重刷包）。

### ② 改测试——root 与非 root **两种身份都要跑一遍**

CI 的 runner 是**非 root**。只跑 root 会得出完全错误的结论。
我在这一条上又交了一次学费，见第 5 节坑 3。

```bash
bash tools/xxx_test.sh                                        # root
setpriv --reuid=65534 --regid=65534 --clear-groups bash ...   # 非 root
```

**以非 root 那一份为准。**

### ③ 仓库可能还没推上去

5 号交付时网络出口对 GitHub 写入受限。我这边同理：
`patches/` 是给主办方合进主仓用的，**别当成"已经合进去了"**。

---

## 0. 一句话

任务6 有 A、B 两面：

- **A 面**：BOSS App（客户端）——把 2/3/4/5 号交付的能力收进一个界面。
- **B 面**：把 5 号点名"⬜ 你的活"之外、但**不做 App 就一次都跑不起来**的那块补掉：
  **manager 身份识别**与**授权弹窗**。

B 面严格说不在"客户端"的字面范围里，但它和 5 号补 init 接管是同一类活：
**不做，A 面就是个空壳。**

---

## 1. 卡住任务6 的那道墙：鸡生蛋

5 号交接文档第 2 节的表里有一格：

| 项 | 状态 |
|---|---|
| BOSS 前端授权 | ❌ 没做，依赖任务 6，接口是 `policy_decide()` |

真正动手才发现，这一格空着不是"没人写 UI"，而是有个绕不过去的死循环：

```
默认策略 = deny
  ↓
App 是个普通 app（uid >= 10000），不是 root、不是 shell
  ↓
/data/adb/boss 是 0700 root 所有 → App 连读都读不了
  ↓
App 没法给自己写一条 policy 规则
  ↓
App 永远被 deny
```

**"写规则"这个动作本身就需要特权，而特权正是你要申请的东西。**

### 1.1 我试过并否掉的三个解法

| 方案 | 为什么否掉 |
|---|---|
| 让 App 启动时 `su -c 'boss policy add app ...'` | 还是得先被放行，循环没解开 |
| 装包时把 uid 预写进 policy.conf | App 的 uid 是安装时才分配的（多用户下还会变），预写不了 |
| 放宽默认策略为 allow | 直接违反"默认拒绝"这个安全语义。为了自己方便把产品的安全模型改松，不行 |

### 1.2 最终解法：manager 由 daemon 侧认定

daemon 认一个"manager uid"并持久化到 `/data/adb/boss/manager.uid`。
判定用**内核给的 uid**（`SO_PEERCRED`），cmdline 只做第二道锁——
架构红线第 2 条：身份只信内核。cmdline 是 zygote 填的包名，
拿它当辅助校验，不是替代。

流程：

```
刷机后首次开机
  → manager.uid 不存在
  → App 首次连 daemon：uid>=10000 且 cmdline 是 com.boss.manager → 抢注成功
  → 之后该 uid 的请求一律自动放行（在 policy_decide 之后、fork 之前）
  → manager.uid 已存在时，别人抢注一律拒绝（顶不掉）
```

为什么"已注册就顶不掉"是硬要求：
否则抢注窗口就从**"刷机后一次"**变成**"每次开机"**，
任何能伪造包名的进程都能在某次开机里抢到 manager。

### 1.3 残留风险（诚实写出来，别当不存在）

manager.uid 还没落盘时，任何 **app uid（>=10000）且 cmdline 等于包名**的进程
都能抢注。窗口是"刷机后第一次开机到 App 首次启动"之间。

真机上想收紧，用 root shell 预先钉死：

```bash
boss policy manager 10123     # 钉死
boss policy manager 0         # 清除，等下次抢注
boss policy manager           # 查当前值
```

**这条退路的存在本身就是解法的一部分**：风险窗口已知、可手动关闭，
比假装它不存在强。

---

## 2. B 面第二块：授权弹窗（prompt 真的等用户）

任务2 定义了三元语义 `allow / deny / prompt`，但 prompt 一直"按拒绝处理"——
`BOSS_PROMPT` 只是个返回码，没人等用户。

### 2.1 为什么用文件而不是共享内存 / 新 socket

daemon 是 fork-per-client 的。**等待中的 handler** 与**答复它的 UI handler**
是两个互不相干的进程：共享内存要额外建映射，新 socket 要新的一套连接管理。
而 `BOSS_DIR` 本来就是 root 私有的 0700 目录，
一个 `.req` + 一个 `.ans` 文件就够了。代价是轮询（200ms 一轮，**只在弹窗期间**）。

### 2.2 两个必须做对的细节

1. **等待期间要同时看着客户端 socket。** 用户没点、app 先退了，请求就该作废，
   否则 handler 白等到超时，还留一个孤儿 `.req` 文件。
2. **id 必须校验。** id 来自 App（不可信输入），会拼进路径。只接受
   `[A-Za-z0-9_.-]`，否则 `a/../../etc/x` 这类能写到任意位置。

### 2.3 出错一律往"拒绝"走

超时（60 秒没人点）、客户端提前退出、建不了请求记录——**全部按拒绝处理**。
弹窗这条路上，宁可让用户重按一次，也不能默认放行。

> 注意：本次放行只管这一次。要长期记住，得写一条真正的策略规则
> （App 里"记住这个应用"勾选后走 `boss policy add app <appId> allow`）。
> `appId = uid % 100000`，跨用户同包同 appId——这是 Android 的约定。

---

## 3. A 面：客户端的设计取舍

### 3.1 协议：扩展语义必须升版本

我给 `boss_request` 加了一个标志位 `BOSS_F_UI`（UI 控制通道：不 fork 子进程，
`command` 字段是控制指令而不是 shell 命令）。

⚠️ **同时把 `BOSS_PROTO_VER` 从 1 升到 2。** 这是任务2 交接时的原话，仍然有效。
不升的后果：老 daemon 收到 `BOSS_F_UI` 不认识这个标志，
会把 `"pending"` 当成一条 shell 命令跑一遍——不报错，只是行为完全不对。

### 3.2 布局一致性用编译期断言钉死

App 侧要重新声明一遍 `struct boss_request`（JNI 层拿不到 `src/boss.h`，
也不该把内部头拖进 App）。一旦上游改了 `boss.h` 而这里没跟上，
表现不是报错，而是"命令发出去、daemon 读到一串错位的值"。

所以 `tools/ipc_layout_test.c` 在**编译期**把两边钉死：
sizeof、每个字段的 `offsetof`、magic、协议版本、响应码、标志位。

> 只比对 sizeof 不够——两个字段换了顺序照样 sizeof 相等。
> 所以每个字段都做 `offsetof` 断言。

### 3.3 主题：Material 3 Expressive（Android 16 原生视觉语言）

你选的最新原生安卓风格，落地就是 M3 Expressive。三个决定：

1. **动态配色优先**（Android 12+）。取色来自壁纸（Monet），不写死品牌色。
   这既符合原生观感，也契合"低痕迹"——不引入一个可识别的 BOSS 色。
   12 以下回退到一组低饱和中性色。
2. **`MotionScheme.expressive()`**。弹簧动效，会略微过冲再回弹。
   Material 官方建议用在主要交互上；BOSS 的授权裁决、挂载摘除需要明确的
   "我动了"反馈，所以用 expressive 而不是 standard。
3. **不碰 Emphasized 排版变体。** `displayLargeEmphasized` 那一族仍是 alpha 里
   相对活跃的部分。原则：**alpha 依赖只吃到真正需要的那几个 API**，其余走稳定面。

⚠️ **版本锁死 `material3 = 1.5.0-alpha27`，且别让 BOM 覆盖掉它。**
Material 3 Expressive 目前只有 alpha（1.4.0 是最后一个稳定版，Expressive API
在 1.5.0 才转稳定）。BOM 会把它拉回稳定版，于是 `MaterialExpressiveTheme` /
`MotionScheme` 全部 unresolved——而报错信息完全不会提示"版本被 BOM 换掉了"。

### 3.4 首页为什么把"劫持"顶在最上面

约束①说得很清楚：劫持没成的话，模块挂载、systemless 清单、脚本、隐藏
在 2SI 设备上**一次都不会执行**。用户看到的症状是"什么都没生效"。

如果首页不把这一条顶出来并且标红，所有人都会去查错误的地方。
所以 `/proc/1/exe` 不指向 boss 时，首页第一张卡就是错误色的告警。

### 3.5 三条别做错的事（5 号的原话，我在 UI 里落实了）

1. **别把隐藏做成"一键隐身"按钮。**
   app 进程与 zygote 共享 mount namespace，无注入时"给某个 app 单独摘挂载"
   做不到——命中名单即摘，共享同一 ns 的进程一起生效。
   隐藏页顶部那张卡把影响范围写在用户眼前。**用错方向的"隐藏"比不隐藏更危险**：
   它给人虚假的安全感。
2. **别把属性伪装当万灵药。**
   它是检测面里的一层，不是全部，不承诺绕过任何第三方完整性/风控判定。
   动态属性会被系统服务改回。模板第一行就写着这个边界，UI 里也复述了一遍。
3. **返回码 3 是"部分应用"，不是错误。**
   它让一条规则在某机型上失效时不至于中断开机。UI 上显示成"失败"会误导。

---

## 4. 接口：文件即契约（沿用 5 号，没造新协议）

```
/data/adb/boss/manager.uid         任务6：App 的 uid（daemon 侧认定）
/data/adb/boss/prompt/<id>.req     任务6：待用户裁决的授权请求
/data/adb/boss/prompt/<id>.ans     任务6：裁决结果（allow / deny）
/data/adb/boss/systemless.conf     任务5：声明式"系统改动"清单
/data/adb/boss/systemless.baseline 任务5：只读分区基线快照
/data/adb/boss/denylist.conf       任务5：隐藏名单
/data/adb/boss/props.conf          任务5：属性伪装清单
/data/adb/boss/modules/<id>/       任务3：模块
/data/adb/boss/boss.log            审计日志
```

**UI 控制通道**（走同一个抽象套接字，`BOSS_F_UI` 标志）：

| 指令 | 用途 |
|---|---|
| `manager` | 我是谁（顺带触发首次抢注） |
| `pending` | 列出待裁决请求（TSV：id / uid / caller / cmd / ts） |
| `allow <id>` / `deny <id>` | 裁决 |

全部要求 manager 身份。判定只用内核给的 uid + zygote 填的 cmdline。

---

## 5. 踩到的坑（别再踩）

1. **`manager.uid` 文件头写了注释，解析时读出来是 0。**
   我为了"人类可读"在文件第一行写了 `# BOSS manager uid`。
   `strtoul` 遇到 `#` 直接返回 0 且 `end == line`，于是"已注册但读出来是 0"，
   表现为**注册成功了、下一次却被判成未注册**。
   修法是逐行跳过注释——和 `policy.conf` 一个道理。
   **注释是给人看的，解析器必须跳过它。**

2. **测试探针的 argv 被 shell 切碎。**
   `probe run echo hello` 在 shell 里是四个参数，只取 `argv[2]` 的话
   传到 daemon 的命令是 `"echo"`，输出自然是空的。
   症状看起来像"IPC 坏了"，其实是探针自己的锅。修法是拼回 `argv[2..]`。

3. **非 root 身份下 bash 读 `/root/.bashrc` 报 Permission denied，**
   噪声混进 stdout，而我的断言是按行解析的——于是全红。
   修法：`env HOME=/tmp bash --noprofile --norc`。

4. **弹窗用例把后续用例全拖住了（这条真红过一次）。**
   我第一版脚本里所有测试 uid 都用了 prompt 规则。
   `default = deny` 的响应是**立即**的，而 prompt 是**等 60 秒**——
   于是第 1 节就把 daemon 的 handler 占住，后面的用例全部 DOWN。
   修法：给弹窗用例单独一个 uid（策略 = prompt），
   其余 uid 走 `default = deny`，立即返回。
   **别让"验证会被拒"的用例走 prompt 规则。**

5. **上游 `build/boss` 的 `BOSS_DIR` 是 `/tmp/boss-test`。**
   直接拿它当测试 daemon 的话，它会去读那边的 policy.conf，
   测试写进自己目录里的策略根本不生效。
   症状和接力须知坑 6（跑着另一个 BOSS_DIR 的二进制而不自知）一模一样。
   修法：**自己编一份夹具**，用 `-DBOSS_DIR` 指到测试目录，产物名独立。

6. **非 root 下 daemon 会把目标身份降级为自身**（daemon.c 有这条分支）。
   这不是 bug：真机上 daemon 永远是 root，这条分支不会进入。
   它让授权链路在 CI 上仍能被验到——**除"提权"以外的整条链路都是真的**。

6. **CI 上"没打补丁"被报成了"协议错位"。**
   独立仓模式下，CI 的 ipc job 直接拿未打补丁的 `BOSS-TASK5/src`（proto **v1**）
   去对 App 侧的 `boss_ipc.h`（**v2**），于是 `_Static_assert` 红：
   `proto version mismatch`。真实原因只是"补丁没合"，但错误信息指向"协议错位"，
   照着它查会去改明明没错的地方。
   两处修法：① CI 每个 job 第一步都 `git apply` 且**硬失败**（不要用
   `continue-on-error`，那等于保证后面一定红且红得莫名其妙）；
   ② 脚本前置预检 `manager.c` / `prompt.c` 与 `BOSS_PROTO_VER +2u`，
   不合就 SKIP 并打印"先打补丁"。

7. **非 root 且没有 sudo 时，`setpriv` 直接失败，报出一串假 FAIL。**
   `setpriv: setresuid failed: Operation not permitted` —— runner 是非 root，
   切 uid 是特权能力，根本做不到。后果是 manager 套件 9 个用例全红，
   而**一个都没真正跑起来**。9 个 FAIL 比不跑更糟：它会让人以为
   manager 自动放行和弹窗都坏了。
   修法：非 root 时先试 `sudo -n`（GitHub runner 有免密 sudo，够用），
   有就整份脚本提权重跑（真机上 daemon 本来就是 root，那份才是高保真的）；
   没有就**老老实实 SKIP**，退出码 0，并说明"能力不具备，不是代码有问题"。
   另加 `BOSS_SUDO_ATTEMPTED` 闸门：sudo 存在但没真给到 root 的容器里，
   没有它就是 exec 自杀式无限重入。

8. **断言消息里的中文被打成八进制转义。**
   `_Static_assert` 的第二个参数写了中文，某些编译器把它输出成
   `\37777777746\37777777624...`，错误信息直接没法读——
   而真出事时最需要看清的就是这一行。**断言消息一律用 ASCII。**

---

## 6. 真机验收清单

```
前置（先做，其余全部依赖它）
[ ] 冷启动后 ls -l /proc/1/exe 指向 boss（劫持成功）；指向原厂 init = 没成
[ ] 加 boss_selinux=0 后仍能正常开机（自救路径可用）
[ ] 刷回原厂镜像仍能开机（这条没了，迭代就停了）

manager 身份（B 面）
[ ] 首次打开 App 后 /data/adb/boss/manager.uid 存在，值是 App 的 uid
[ ] App 里所有功能都能执行（不再显示"被拒绝"）
[ ] 用另一个 app uid 冒充包名，抢注失败（顶不掉）
[ ] boss policy manager 能查 / 能钉死 / 能清除

授权弹窗（B 面）
[ ] 第三方 app 请求 root 时，App 的"待裁决"页出现一条
[ ] 点"允许"后命令真的跑起来了；点"拒绝"后返回 DENIED 且命令没执行
[ ] 60 秒不点 → 按拒绝处理（不会默认放行）
[ ] 调用方提前退出 → handler 不白等，prompt 目录不留孤儿 .req
[ ] 勾选"记住这个应用"后，boss policy show 里多一条 app 规则

界面（A 面）
[ ] boss systemless status 的数字与首页一致
[ ] boss hide mounts 的条数与"暴露面"卡片一致
[ ] boss hide denylist list 与隐藏名单页一致
[ ] boss module list 与模块页一致
[ ] 装一个带 system/ 的模块后 verify 仍判"干净"（差异不定罪）
[ ] 审计日志页能看到刚才的授权记录，且 App 自己的轮询没刷进去

隐蔽性回归（每批都要）
[ ] 没有引入新的可被检测的属性
[ ] /system 的 hash 与刷机前一致
[ ] App 不申请任何权限（Manifest 里应当一个 uses-permission 都没有）
```

---

## 7. 已知风险与没验的部分

诚实列出来，避免下一位把"没验过"当成"验过"：

| 项 | 状态 | 说明 |
|---|---|---|
| **抢注窗口** | 已知、可手动关闭 | 刷机后首次开机到 App 首次启动之间；`boss policy manager <uid>` 可钉死 |
| **manager 判定的 cmdline 那道锁** | 未上真机 | 真机上 cmdline 由 zygote 填；沙盒里是用 `exec -a` 演的 |
| **弹窗的实际时序** | 未上真机 | 轮询 200ms 一轮，真机上取决于 App 前台轮询间隔 |
| **M3 Expressive 的 alpha 依赖** | 已知 | 1.5.0-alpha27；官方明确"没有生产就绪的 Expressive"，要吃这个风险得自己决定 |
| **UI 解析器的护栏** | 依赖 `tools/contract_test.sh` | 上游改了 printf 格式而 Parsers.kt 没跟上，红的是那份脚本 |
| **JNI 层** | 未上真机 | 协议布局在编译期钉死了，但真机上的 `.so` 加载没验过 |

**沙盒里已经验到的**：

| 套件 | 项数 | 覆盖 | 结果 |
|---|---|---|---|
| `tools/ipc_layout_test.c` | 编译期断言 | sizeof / offsetof / magic / proto / 响应码 / 标志位 | PASS |
| `tools/ipc_host_test.c` | 10 | 探活、输出、退出码回传、CRLF 归一、大输出 | PASS（root 与非 root 各一遍） |
| `tools/manager_flow_test.sh` | 14 | 拒/抢注/顶不掉/自动放行/弹窗放行/弹窗拒绝/不留痕 | PASS |
| 上游 `smoke_test.sh` | 28 | 主链路回归 | PASS（无回归） |
| 上游 `systemless_test.sh` | 16 | 任务5 A 面 | PASS（无回归） |
| 上游 `hide_test.sh` | 17 | 任务5 B 面 | PASS（无回归） |
| 上游 `hijack_test.sh` | 8 + 2 SKIP | 劫持布置 | PASS（无回归） |

---

## 8. 上手路径

```bash
# 0) 先把 daemon 侧的补丁合进主仓
git apply patches/0001-task6-manager-and-prompt.patch
make test

# 1) 协议布局断言（改了 src/boss.h 这里必红）
BOSS_SRC=<boss 主仓>/src bash tools/run_ipc_test.sh

# 2) manager 与弹窗端到端
BOSS_SRC=<打过补丁的 src> bash tools/manager_flow_test.sh

# 3) CLI 输出契约（App 解析器的护栏）
BOSS_BIN=<主仓>/build/boss bash tools/contract_test.sh

# 4) 真机第一件事（约束①）
ls -l /proc/1/exe

# 5) 客户端
./gradlew :app:assembleDebug
adb install app/build/outputs/apk/debug/app-debug.apk
```

新增/改动的文件：

```
app/src/main/cpp/boss_ipc.h        App 侧协议镜像（与 src/boss.h 一一对应）
app/src/main/cpp/boss_ipc.c        协议客户端：连接 / 握手 / 收输出 / 收退出码
app/src/main/cpp/boss_ipc_jni.c    JNI 胶水（只搬参数，不做任何决策）
app/src/main/cpp/CMakeLists.txt
app/src/main/java/.../core/BossIpc.kt      协议封装与返回码语义
app/src/main/java/.../core/BossCli.kt      CLI 契约层（命令字符串只写一遍）
app/src/main/java/.../data/Models.kt
app/src/main/java/.../data/Parsers.kt      CLI 输出解析器（纯函数）
app/src/main/java/.../data/BossRepository.kt
app/src/main/java/.../ui/BossViewModel.kt
app/src/main/java/.../ui/MainActivity.kt
app/src/main/java/.../ui/screens/*.kt      首页 / 模块 / 隐藏 / 授权
app/src/main/java/.../ui/theme/Theme.kt    Material 3 Expressive
app/src/main/AndroidManifest.xml           零权限
src/manager.c           （补丁）manager 身份识别
src/prompt.c            （补丁）授权弹窗
src/boss.h              （补丁）PROTO_VER 1→2、BOSS_F_UI、manager/prompt 声明
src/daemon.c            （补丁）UI 通道、manager 自动放行、prompt 真等用户
src/policy.c            （补丁）boss policy manager
tools/ipc_layout_test.c     协议布局编译期断言
tools/ipc_host_test.c       IPC 端到端
tools/ipc_probe.c           测试用探针
tools/run_ipc_test.sh
tools/manager_flow_test.sh  manager + 弹窗端到端
tools/contract_test.sh      CLI 输出契约护栏
patches/0001-task6-manager-and-prompt.patch  daemon 侧补丁
```

---

## 9. 别浪费时间的地方

- **别给 App 加任何权限。** Manifest 里应当一个 `uses-permission` 都没有。
  多一个权限就是多一份痕迹，而痕迹是这个产品最贵的东西。
- **别给 su 客户端加特权**（架构红线第 1 条）。App 也同理。
- **别为了跑测试去拆 `needs_root`**（5 号第 4.3 条，原话照抄）。
- **别信"编译通过就行"**（接力须知 4.5）。协议布局那道断言同理：
  它编译期不报、运行期不崩，只是静默错位。
- **别往 `/system` 落任何东西**（这是 BOSS 与 Magisk 定位差异的根本）。
- **别在注入阶段造 `/su`、`/system/bin/su`**（兼容性留给 App 按需挂载）。
