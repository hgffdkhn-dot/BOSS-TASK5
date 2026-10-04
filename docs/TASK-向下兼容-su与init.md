# BOSS · 向下兼容（su 与 init）

> 本轮只动 **su 与 init** 这两块（产品决定由 su 的打造者来补兼容）。
> 范围之外的（resetprop 属性区、模块挂载、SELinux 规则）不在本轮验收里，
> 边界见最后一节。

---

## 1. 为什么必须做

上一轮把 2SI（Android 10+）的 SwitchRoot 劫持补齐之后，整条执行链是
**为现代设备写的**。老设备的差别不是"少个功能"，而是**走法根本不同**：

| 差别 | 现代 2SI 设备 | 老设备（非 SAR，Android ≤7/8） |
|---|---|---|
| ramdisk 命运 | 切根后消失 | 就是 `/`，全程有效 |
| ramdisk 里的 rc | 只有 early-init 能跑 | post-fs-data 之后照样跑 |
| 要不要劫持 | 必须 | **不能做**（无效且有害，见 3.2） |
| rc 语法 | `exec_background` / `exec <seclabel>` 随便用 | 这两种是 8.0+ 才有，老 init 会拒整段 |
| su 命令行 | 我们自己定 | 老 root 应用会传 Magisk 那套参数 |

不做兼容的后果分级：
- **变砖级**：rc 解析失败 → 老机器上 BOSS 一个动作都不做（不砖机，但不工作）
- **功能级**：老应用传 `-M` / `su root -c` → 被当成错误或吃掉
- **玄学区**：两份 boss 版本错开 → su 报"内部错误"，看起来像权限问题

---

## 2. 做了什么

| 项 | 改动 | 文件 |
|---|---|---|
| 协议双向兼容 | daemon 接 `[1, 2]` 而不是只认 2；客户端被拒时自动降 v1 重发 | `src/daemon.c` / `src/client.c` / `src/applet.c` |
| 协议版本可观测 | 审计日志加 `proto=vN`；混版本部署时这是唯一线索 | `src/daemon.c` |
| su 命令行兼容 | `-v/-V` 版本、`-M/--mount-master`、`-Z/--context`、`--command`、位置用户（`su root -c`）、`-c` 后多参数拼接、未知选项忽略 | `src/client.c` |
| 布局探测 | 新增 `boss init probe`，输出 LAYOUT / HIJACK_NEEDED / RAMDISK_RC_EFFECTIVE | `src/bossinit.c` |
| 按布局自适应 | 非 2SI 布局直接跳过劫持（老设备上劫持会遮住 `/sdcard`） | `src/bossinit.c` |
| 现场覆盖开关 | `boss_hijack=0/1`（cmdline，真机自救）+ `BOSS_LAYOUT=`（离机测试） | `src/bossinit.c` |
| 劫持落点重做 | 不再碰 `/sdcard`；改挂"切根后成为 `/system/bin/init`"的真实路径，且挂载后按 mountinfo 验收 | `src/bossinit.c` |
| 劫持重试时机 | rc 触发点 `early-init` → `early-init` / `fs` / `post-fs`，幂等 | `payload/init.boss.rc` |
| rc 语法降到老 init | 全程 `start` + `service`，不用 `exec_background`、不写 `seclabel`，服务名 ≤16 | `payload/init.boss.rc` |
| payload 下限 | `min_api` 26 → 23 | `payload/manifest.json` |
| 验收 | 新增 `tools/compat_test.sh`（28 项），CI 加 `compat` job | `tools/` `.github/` |

---

## 3. 三个关键决策

### 3.1 协议：双向兼容，但不能"静默降级"

```c
/* daemon 侧：不再要求等于当前版本 */
if (req.magic != BOSS_MAGIC ||
    req.version < BOSS_PROTO_MIN || req.version > proto_max) { ... 拒绝 }

/* 客户端侧：被拒就按下一个版本重发一次 */
for (unsigned v = BOSS_PROTO_VER; v >= BOSS_PROTO_MIN; v--) { ... }
```

理由是真机上 **ramdisk 里的 boss 与 `/data/adb/boss/boss` 是两份二进制**，
换包 / OTA / 手动升级都会让它们错开。严格相等检查的结果是：
旧 su 撞上新 bossd（或反过来）时整台机器 root 静默失效，
症状是"su 还在，但所有命令都报内部错误"——没人会联想到版本。

v1 与 v2 的 `struct boss_request` 布局**完全一致**（v2 只是多用了几个标志位），
这是"能兼容"的前提。**别改结构体开头那几个字段的顺序。**

两个护栏：
- UI 控制通道只对 `version >= 2` 生效，v1 请求带上那个位会被清掉
  （否则一条 shell 命令会被当成控制指令）。
- 降级会在日志里留下 `proto=v1`，不是静默发生的。

### 3.2 劫持：老设备上**必须跳过**，不是"尽力做"

这是本轮最重要的判断。在 2SI 设备上不劫持 → BOSS 永远不被叫起来；
但在**非 SAR 老设备**上劫持 → 那个 init 根本不会去 exec `/system/bin/init`，
劫持无效，而我们还要把 boss 二进制 bind 到 `/sdcard` 上——
老设备上 `/sdcard` 常常是真实目录，这一下用户的内部存储就没了。

所以：先探测，不是 2SI 就直接跳过并返回 0（不拖住 init）。

判定只用运行时看得见的事实：

```
/first_stage_ramdisk 存在          → 2SI
/system/bin/init 可执行            → 有二阶段可劫持（/system 挂载后才有，作补充证据）
/init 里含 "/system/bin/init" 串   → 这个 init 会去 exec 第二阶段
都没有                              → legacy_root（rc 全程有效，不需要劫持）
```

**判定错了怎么办**：cmdline 加 `boss_hijack=0` 或 `=1` 就能现场改方向，
不用重刷镜像。这和已有的 `boss_selinux=0` 是同一条自救思路——
早期注入在某个机型上出问题时，靠它退回原厂路径。

### 3.3 rc：语法压到 Android 5.0 的下限

两种写法老 init 不认识：`exec_background`（8.0+）、
带 `seclabel/user/group` 的 `exec`（8.0+）。
老 init 遇到不认识的命令/选项会**拒绝整段**，
日志里只有一句 parse error，表现却是"BOSS 一个动作都没做"——
排查方向极易跑偏到挂载或策略上。

所以整份 rc 用最老那套：`on <trigger> / start <service>` +
`service … / class / user / group / disabled / oneshot`，
并且**不写 seclabel**（服务继承 init 的域 `u:r:init:s0`，正好是我们要的）。
服务名一律 ≤16 字符（老 init 对服务名长度有限制）。

代价：`start` 是异步的，而 `exec` 是同步的。劫持那一步原本用 `exec`
是为了"确定性地在切根前做完"，现在改成 `start boss_early` 之后
理论上存在"还没挂上就切根"的可能。缓解：`boss_early` 挂在 `early-init`
（切根前最早的 trigger），且 `class core`；真机上若发现劫持偶发失效，
第一件事是 `dmesg | grep boss` 看事件顺序，而不是改回 `exec`（改回去 6/7 就全废）。

---

## 4. 新增的 CLI

```bash
boss init probe          # 上机第一件事：确认自己在哪种布局上
# LAYOUT:2si|legacy_root|unknown
# HIJACK_NEEDED:0/1
# RAMDISK_RC_EFFECTIVE:0/1
# REAL_INIT:/init.real
```

su 新增/放宽的参数（都是为了老应用）：

| 参数 | 行为 |
|---|---|
| `-v` / `-V` | 打印版本（老脚本两种都写） |
| `-M` / `--mount-master` | 接受并按"已满足"处理（bossd 由 init 拉起，本就在全局 mount ns） |
| `-Z` / `--context` | 指定目标 SELinux domain |
| `--command` | `-c` 的长写法 |
| `-c a b c` | 后面的 token 全部拼成一条命令（老应用常写 `su -c ls -l`） |
| `su root -c …` / `su 0 -c …` | 位置参数当目标用户 |
| 未知 `-xxx` | 忽略，不报错（老应用传得比我们实现的多） |

---

## 5. 验收

```bash
bash tools/compat_test.sh     # 28 项：rc 语法 / 布局探测 / 协议与命令行
bash tools/smoke_test.sh      # 28 项主链路（回归）
bash tools/hijack_test.sh     # 劫持布置（已加了"老布局必须跳过"一项）
```

本轮实测（root 与非 root 都跑过）：compat 28/28、smoke 28/28、
hijack 10 PASS、component 36、systemless 16、hide 17、contract 18、
manager_flow 全过、IPC 10 项全过。

`compat_test.sh` 里两条是真端到端，不是摆设：
- **老客户端 → 新 daemon**：用 `-DBOSS_PROTO_VER=1` 编一个真·v1 客户端去连 v2 daemon
- **新客户端 → 只认 v1 的老 daemon**：用 `BOSS_PROTO_MAX=1` 起一个老 daemon，
  验证客户端被拒后自动降级，并确认日志里先有一次"拒绝 v2"再有一次 `proto=v1`

> ⚠️ 两种身份都要跑。本轮就撞到一次：`smoke_test.sh` 里 `/tmp/boss-props`
> 没有像 `/tmp/boss-test` 那样做"清不掉就报错"的保护，root 跑完留下的目录
> 让 nobody 的 resetprop 整段读不到合成属性区，6 项全红——看着像"属性改写
> 改坏了"，其实是权限。已补上同样的清理自检，并把 `--file` 用的临时文件从
> 全局 `/tmp/boss-test.prop` 挪进 `TEST_DIR`。

> ⚠️ 编"老客户端"能成立的前提：`boss.h` 里的 `BOSS_PROTO_VER` 用 `#ifndef` 包住。
> 写成无条件 `#define` 时，命令行的 `-DBOSS_PROTO_VER=1` 会被 header 盖掉，
> 编出来的"老二进制"其实还是 v2——**测试会假绿**。这条踩过一次。

---

## 6. 本轮修掉的第二个真 bug：劫持的"假成功"（外部开发者指出）

上一版 `hijack-prep` 把 bossinit bind 到 `/sdcard`。开发者指出：`/sdcard` 是 init
在 post-fs 前后才建的 symlink，**early-init 时不存在**。实际执行序列是：

```
access("/sdcard") != 0  →  open("/sdcard", O_CREAT)  →  造出一个普通文件
mount(self, "/sdcard")  →  挂到普通文件上是合法的  →  返回 0  →  mounted=1
```

后果比"没生效"更糟，一共三层：

1. bossinit 落在 `/sdcard` 这个空文件上，切根后它在 `/system/sdcard`——
   **不是** `/system/bin/init`，劫持根本没发生；
2. `mounted=1` 让退路 1（`/storage/self/primary`，那条才是对的）与
   退路 ③（hexpatch）全部被跳过，**且不打任何失败日志**；
3. `/sdcard` 被占成一个普通文件后，init 稍后那句
   `symlink /storage/self/primary /sdcard` 会因 EEXIST 失败——
   **用户的内部存储直接没了**。

本机已复现确认：跑完 `/sdcard` 变成一个 160KB 的普通文件。

**修法三条**：

| # | 做法 | 为什么 |
|---|---|---|
| 1 | 不再碰 `/sdcard`，也不再铺 `/storage/self/primary` | 两个都是 init 稍后自己要建的路径，提前占住就是 EEXIST 冲突 |
| 2 | 挂载点必须是**已存在的普通文件** | `/system` 在第一阶段早期还没挂载，那时挂必然 ENOENT——这是"时机未到"，要留给后续 trigger 重试，不能当失败，更不能谎报成功 |
| 3 | 挂载后**验收**：扫 `/proc/self/mountinfo` 确认目标路径上真多了一条挂载 | `mount()` 返回 0 只说明"挂上了"，不说明"挂对了地方" |

同时把 rc 里的触发点从 `early-init` 一处扩到 **`early-init` / `fs` / `post-fs`
三处**：劫持落点位于 `/system` 上，早期 `/system` 没挂载，真正生效的往往是更晚
的那一趟。hijack-prep 自身幂等（已挂载就跳过），多跑几次没有副作用。

### 顺带：`mntinfo-guard` 的误报与加固

改完上面这些之后，CI 的「挂载解析单一来源自检」报了
`ERROR: 挂载表解析出现了第二份实现`——但我的 `bossinit.c` **并没有**再写一份解析，
它只是调用了 `boss_mount_scan()`，注释里提了一句"靠挂载表验收"。

原因是那条守卫按 `grep -l mountinfo src/*.c` 计数，把注释里的提词也算成了实现。
**真正的第二份实现必然出现 `/proc/self/mountinfo` 这个路径字面量**，所以判据改成：

```bash
grep -rlF "/proc/self/mountinfo" src/*.c      # 真的打开了这个文件
grep -rl  "int boss_mount_scan"  src/*.c      # 扫描函数只定义一次
```

反向验证过：临时塞一个含 `fopen("/proc/self/mountinfo")` 的假文件，两条判据都从
1 变成 2，会被抓住；删掉后回到 1。也就是说加固没有削弱它——
**真重复照样红，只是不再被注释里的字眼误伤**。

同时把 `bossinit.c` 的注释改写成显式指向 `boss_mount_scan()`，
免得下一位读者以为那里另有一套解析。

### 顺带挖出的第三个 bug：`realpath` 会 abort

改用 `realpath()` 解析目标路径时，最初给了 512 字节缓冲区，结果运行直接
`*** buffer overflow detected ***` 然后 abort。glibc 在开了 `_FORTIFY_SOURCE`
时会检查目的缓冲区，realpath 需要 **PATH_MAX** 大小。

**这个 abort 若发生在第一阶段 init 里就是开机直接挂**，比劫持失败严重得多。
现已统一按 `PATH_MAX` 开辟，并在 `target_probe()` 里对"调用方给小了"直接拒绝。

## 7. 本轮修掉的一个真 bug（栈越界写）

为了兼容老应用 `su -c ls -l`（命令不带引号）的写法， su 要把 `-c` 之后的所有
token 拼成一条命令。第一版是这么写的：

```c
off += (size_t)snprintf(cmd + off, sizeof(cmd) - off, "%s", argv[i]);   /* 错 */
```

`snprintf` 返回的是"**本应**写入的长度"，不是实际写入数。发生截断时它大于可用
空间，于是 `off` 越过 `sizeof(cmd)`；下一轮 `sizeof(cmd) - off` 是 `size_t`，
**无符号下溢成一个巨大值**，那一次的 `snprintf` 就变成越界写。

实测（独立 PoC，cmd 为 1024 字节）：

```
i=3  追加后 off=1809  >>> 已越过缓冲区边界 <<<
i=4  进入时 off=1809  room=18446744073709550831     <- 下溢
```

哨兵验证更能坐实：把 16 字节哨兵紧跟在 `cmd[1024]` 之后，构造"写入起点刚好
落在哨兵内"的用例，旧逻辑把哨兵踩成 `SSSSSSScccc...`——确实写到了缓冲区外。

修法是单独一个 `cmd_append()`，按"实际能容纳的量"推进，截断时直接把 `off` 钳到
`cap`，保证 `off <= cap` 恒成立。修复后同一用例哨兵完好。

顺带一提：ASAN/UBSAN **没能**捕获这个越界（栈数组插桩没覆盖到"写入起点直接跳过
redzone"这种情况），所以别把"ASAN 没报"当成"没有内存问题"。

> ⚠️ 测试脚本里的中间输出一律写 `$TEST_DIR`，不要放 `/tmp/xxx` 这类全局
> 固定路径。root 跑过一次之后，非 root 会 Permission denied，症状是
> "命令没输出 → 判定失败"，看着像功能坏了，其实是权限。本轮踩到三次
> （`/tmp/boss-props`、`/tmp/boss-test.prop`、`/tmp/boss-hij.out`），已全部收敛。

## 8. 边界：这一轮**没有**覆盖什么

| 事项 | 状态 | 说明 |
|---|---|---|
| resetprop 在 Android ≤7 | ❌ 未验 | 属性区格式在 8.0 重写过（trie + `property_contexts` 多区域）。老格式能不能读，没在真机上验过，别当成能用 |
| 模块挂载在老设备 | ❌ 未验 | Magic Mount 依赖 tmpfs/bind，理论上老内核更宽松，但没验 |
| SELinux on ≤7 | ❌ 未验 | 规则集是按 8.0+ 的 domain 名写的 |
| `min_api` 下调的含义 | ⚠️ 部分 | 只表示"注入不再被 min_api 拦"；su/init 的老设备路径按设计成立，其余组件仍是 8.0+ 语义 |

**给下一位的一句话**：`min_api=23` 是"允许注入到老设备"，不是"全部功能在老设备上可用"。
真机上拿到 Android 6/7 机器时，先 `boss init probe` 看 LAYOUT，再逐项试组件，
别一次全开。
