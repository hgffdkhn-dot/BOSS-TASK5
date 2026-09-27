# BOSS · 任务5：无修改系统逻辑与后续特典逻辑

> 给任务6（客户端），以及回看这份文档的 su 侧 v0.2 / 任务3、4。
> 前置阅读：`docs/HANDOFF-接力须知.md`（红线与坑）、`docs/TASK3-组件交付与接力.md`、
> `docs/TASK4-SELinux-解决与规则注入.md`（尤其是它的 7.1 节）。
>
> 这里只写**从别处看不出来**的东西：为什么"无修改"必须能自检才算数、
> 隐藏功能在 Android 上的真实边界在哪、以及我替下游补掉的那个前置缺口。

---

## 0. 一句话

任务5 有 A、B 两面：A 面是**怎么改系统却不改系统**（声明式清单 + 可自检），
B 面是**改完之后还能不能被看见**（隐藏名单 / 暴露面治理 / 属性伪装）。

外加一件不在原计划里、但不做就一切白费的活：**把 v0.2 的 init 接管补完**。
任务4 的 7.1 节已经点名这是任务5 会撞上的那堵墙——我到的时候墙还在，
现在墙拆了一半（代码补完，真机待验）。

---

## 1. 接手时的现状：三件事都缺同一块地基

| 部分 | 接手时 | 现在 |
|---|---|---|
| 弹药（resetprop + Magic Mount） | ✅ 任务3 已交付 | 沿用，一行没改 |
| **改动的单一事实来源** | ❌ 散落在 `service.d` 脚本里 | ✅ `systemless.conf` |
| **"无修改"可自检** | ❌ 只是口号 | ✅ `boss systemless verify` |
| **隐藏 / 暴露面治理** | ❌ 完全没有 | ✅ `boss hide` |
| 属性伪装 | ❌ | ✅ 模板 + 清单引擎 |
| **2SI 上能不能被拉起来** | ❌ 缺 mount、缺 `/init.real`、缺触发者 | ✅ 已补，真机待验 |

最后一行的三块缺失，前两块是任务4 早就点名的（`cmd_hijack_prep` 没有 `mount()`、
没人备份 `/init.real`），第三块是我分析时发现的：

**即使劫持成功了，2SI 设备上也没有人会触发 post-fs-data 编排。**
`init.boss.rc` 被 veritpath import 进 **ramdisk** 的 `/init.rc`，而 2SI 设备
第一阶段结束就 SwitchRoot + chroot，那份 rc 连同 ramdisk 一起消失——
第二阶段 init 根本没见过它。所以模块挂载、systemless 清单、脚本全都不会跑。

症状和前两块缺失一模一样：**代码写完、单测全绿、真机一次不执行**。

---

## 2. A 面：无修改系统逻辑

### 2.1 为什么"自检"是这个任务的本体

"我们没有修改系统"这句话，如果不能变成一条可执行的断言，它就只是宣传语。
真机上一旦出问题（某模块没生效、某属性没改对、开机慢 40 秒），
没有自检就只能靠猜——而猜的方向往往是错的。

所以 `boss systemless verify` 做的不是"感觉没问题"，而是三类可判定的事实：

| 检查 | 断言 | 为什么是这条 |
|---|---|---|
| ① 挂载来源 | 每条 BOSS 引入的挂载，源必须来自 `/data`（或 tmpfs 覆盖层） | 这是"无修改"的直接定义 |
| ② 只读分区状态 | `/system` `/vendor` 等仍为 `ro` | 被 remount rw 就等于"系统可以被改"，不管是谁改的 |
| ③ 基线比对 | 与刷机前快照的差异处数 | 新增/删除/大小变化一眼可见 |

三个判定细节值得单独说，都是会静默出错的：

- **只读状态要按逗号精确匹配。** 子串匹配会把 `errors=remount-ro` 认成 `ro`，
  于是"分区早就可写了"被判成"一切正常"。
- **基线只记路径+大小+mtime，不记内容 hash。** `/system` 上万个文件，
  全量 sha256 在 post-fs-data 这个阻塞阶段跑不完；而"多文件/少文件/大小变了"
  已经足够回答"有没有人往系统分区落东西"。
- **③ 发现差异不定罪。** 模块挂载本来就会让系统分区"看起来"变了，
  定罪要看①的挂载源。把差异直接判成失败，会在装了模块的机器上天天误报。

### 2.2 为什么清单必须是声明式的

散在 `service.d` 里的脚本有三个绕不过去的问题：

1. **没有单一事实来源。** "这台机器到底被改了什么"分散在 N 个文件里，
   于是也就无法自检——2.1 节的整个逻辑无从建立。
2. **时序靠人记。** post-fs-data 是阻塞阶段，属性改写必须走
   `resetprop -n`（直写属性区、不触发 `on property:`），否则死锁开机约 40 秒
   （任务3 坑 4 的原话）。交给脚本作者记这件事，迟早有人忘。
   所以 `systemless apply` 按 `--stage` 自动决定，调用方记不住也没关系。
3. **一条写错拖垮一批。** 清单解析对坏行只警告不中断——
   一条语法错误不该让整份清单失效。

### 2.3 一条红线

本子系统只做两件事：**属性层改写**与**文件层覆盖**。
任何要往只读分区落文件的需求都是越界，请打回——"无修改"是靠这条边界成立的。
（任务3 交接文档 4.2 也是这么划的，我只是把它在代码里落实成不可绕过。）

---

## 3. B 面：特典逻辑

三件特典按代价从小到大排：① 列出暴露面 → ② 摘除 → ③ 属性伪装。

### 3.1 先看得见，才谈得上治理

`boss hide mounts` 把 BOSS 引入的挂载列出来。判定有两类，缺一不可：

- 挂载源在 `/data/adb/boss` 下（模块文件、镜像目录）；
- **tmpfs 盖在只读分区上**（Magic Mount 的覆盖层，`module.c` 的 `mount_node` 产物）。

第二类是最容易漏的：它的 source 字面值是 `"tmpfs"`，按源判断会一条都认不出来——
而它恰恰是最该摘掉的一类。

### 3.2 隐藏的真实边界（这段请读完再用）

**Android 的 app 进程与 zygote 共享 mount namespace。**
所以"给某个 app 单独摘挂载"在**无注入**的前提下做不到：
我们在 A 进程的 ns 里 umount，同 ns 的 B 进程也一起看不见了。

真正按进程隔离要走 zygisk 式注入（在 app 进程内 `unshare(CLONE_NEWNS)` 后再 umount），
那属于后续任务。这里做的是无注入版本：命中名单就摘，代价是共享 ns 的进程一起生效。

我把这个取舍写进了代码注释、命令帮助和本文，而不是悄悄改语义——
**用错方向的"隐藏"比不隐藏更危险**：它给人虚假的安全感。

三个实现细节：

- **必须 fork。** `setns` 会**改变本进程**的命名空间归属，直接在本进程里做
  等于把自己也挪过去，之后再也回不到原来的挂载视图（init 阶段尤其致命）。
- **换 ns 之后必须重新扫挂载表。** 目标 ns 的挂载视图与我们不同，拿宿主的
  列表去 umount 会大面积 `EINVAL`。
- **用 `umount2(MNT_DETACH)` 而不是 `umount`。** 目标挂载可能正被占用，
  普通 umount 会 `EBUSY`；lazy 卸载立刻让它从路径空间消失（检测看不到），
  真正的回收等引用归零。我们要的是"看不见"，不是"立刻释放"。

**去重要用 (pid, starttime) 而不是 pid。** pid 会复用：一个短命进程退出后
pid 被新进程拿走，只记 pid 就会以为"已经处理过"而漏摘——
正好是隐藏功能最怕的漏网。

### 3.3 属性伪装的边界

`boss hide props` 生成的模板里，第一行说明就写着：
**这是检测面里的一层，不是全部；它不承诺、也不以保证绕过任何第三方完整性/风控判定为目标。**

BOSS 主打隐蔽是产品定位，但"隐蔽"不等于"欺骗用户"。
动态属性（`sys.usb.state` 之类）会被系统服务随时改回，写了也会漂——
这些我都写进模板注释，而不是让人拿去当万灵药。

---

## 4. 替所有人补掉的前置缺口（v0.2 init 接管）

这一节严格说不是任务5 的活，但不做任务5 就一次都不会被执行。

### 4.1 三块缺失

| 环节 | 补法 |
|---|---|
| `cmd_hijack_prep` 的 bind mount | 直接调 `mount(2)`（第一阶段没有 toolbox，fork `/system/bin/mount` 必然 127） |
| `/init.real` 备份 | 硬链接 + bind mount，**两件都要做** |
| 切根后谁来触发 post-fs-data | `spawn_boot_waiter()`：等 /data 就绪后自己触发一次 `boss boot post-fs-data` |

**`/init.real` 为什么两件都要做：**

- 硬链接保住 inode 上的 `security.selinux`（`init_exec` 标签）。任务4 的 6.3 点名过：
  跳过原厂 `selinux_setup` 后，init 第二阶段的域靠 exec 一个带该标签的文件完成，
  标签丢了域就错了。
- bind mount 保证切根后仍可见。ramdisk 里的**文件**在 chroot 后消失，
  而**挂载**会被 SwitchRoot 移动到 `/system` 下。只做硬链接等于没做——
  切根后 `/init.real` 依然不存在，`cmd_stage2` 走到 `execv` 前就返回 127。

### 4.2 触发者为什么是"轮询"而不是 init 原生 trigger

`spawn_boot_waiter()` 在 `/data` 就绪后自己触发一次编排。
代价：比 init 原生的 post-fs-data 晚几十毫秒到几秒（取决于 /data 解密耗时）。

对绝大多数模块无影响；对"必须在 init 挂载 /data 那一瞬间生效"的极少数模块，
请改走 `service.d`，别赌这个时序。这个取舍写在代码注释里，不藏。

### 4.3 只能真机验，别指望写完就绿

沙盒里 `mount` 需要特权，也没有 SwitchRoot 可观察，本地最多验到"命令拼对了"。

**验它有没有成的最快方法**（任务4 与接力须知都强调过，这里再抄一遍因为它最省时间）：
切根后 `ls -l /proc/1/exe` —— 是我们自己就说明劫持成功，是原厂 init 就没成。
**这比翻日志快得多，那个阶段日志可能一个字节都没有。**

`hijack-prep --dry` 是给这件事准备的：真机上先演练一遍看它打算挂到哪，
再决定放不放手。

---

## 5. 踩到的坑（别再踩）

1. **`boss_copy(dst, n, src)` 在 dst == src 时是未定义行为。**
   我在隐藏名单的就地过滤里踩了：`boss_copy(list[w++], ..., list[i])`，
   `w == i` 时是自拷贝，glibc 上直接把自己清成空串——
   症状是"**删掉一项，整个名单被清空**"。
   修法两处：调用方 `if (w != i)`；更根本的是在 `boss_copy` 里挡一道。
   **后者更重要**——这个函数全项目都在用，让每个调用方各自记住这件事不现实。
2. **测试脚本不能断言"路径不存在"，要断言"没有新增"。**
   劫持测试的真跑会留下 `/sdcard`、`/storage/self`，
   脚本跑第二遍时"dry run 无副作用"这项必然误报。改成基线与执行后对比。
3. **常驻进程不能用 `run_tool`。** 它会 `waitpid` 到子进程退出，
   而守护进程永不退出——表现为 service 阶段卡死，后面的脚本一次都跑不了。
   所以 `boot.c` 里单独写了 `spawn_detached()`（fork + setsid + 不 wait）。
4. **空名单不要起守护进程。** 为一个空名单常驻一个轮询进程纯属浪费，
   而且**空转的守护进程本身也违背"低痕迹"**——扫描动作越是看不见越好，
   没目标时就不该有动作。
5. **`resetprop` 读出来是 `[ro.debuggable]: [0]` 这种格式**，
   测试里按整行相等断言必然失败。要按值匹配。
6. **清理构建产物别用 `rm -rf build`。** 我在收尾时顺手这么干了一次，
   把 `build/build-ndk.sh` 一起删了——**就是 `.gitignore` 注释里警告的那个坑**，
   只不过从"提交不上去"变成了"文件没了"。CI 的 android job 第一步就调它，
   而且本地永远是绿的（脚本一直躺在自己的工作区里，只有别人 clone 下来才炸）。
   正确姿势是 `make clean`：它只删 `build/boss`、`build/boss-static` 这些具体产物，
   不碰脚本。

---

## 6. 给任务6（客户端）的接口

- **CLI**：`boss systemless <子命令>`、`boss hide <子命令>`，
  返回码沿用任务4 的语义（`0` 全应用 / `3` 部分应用 / `1` 失败 / `2` 无能力）。
  **部分应用不是错误**，它让一条规则在某机型上失效时不至于中断开机。
- **文件即契约**：`/data/adb/boss/` 下新增的四个文件就是 App 的读写面——
  `systemless.conf`、`systemless.baseline`、`denylist.conf`、`props.conf`。
  它们与模块目录同级，App 直接读写即可，不需要新协议。
- **状态展示**：`boss systemless status` 一行出清单配置与挂载统计；
  `boss hide mounts` 出暴露面清单；`boss hide denylist list` 出隐藏名单。
- **协议**：扩展 `struct boss_request` 时**必须同时升 `BOSS_PROTO_VER`**，
  否则新旧版本二进制静默错位（任务2 交接时的原话，仍然有效）。
- **别把隐藏做成"一键隐身"按钮。** 3.2 节的 ns 共享限制决定了它不是无代价的，
  UI 上应当说清影响范围，而不是给一个看着万能的开关。

---

## 7. 真机验收清单

```
劫持（先做，其余全部依赖它）
[ ] 冷启动后 ls -l /proc/1/exe 指向 boss（劫持成功）；指向原厂 init = 没成
[ ] ls /init.real 存在，且它是原 init 的硬链接/挂载（inode 上有 init_exec 标签）
[ ] boss init hijack-prep --dry 在真机上能跑（先看它打算挂哪）
[ ] 加 boss_selinux=0 后仍能正常开机（自救路径可用）
[ ] 刷回原厂镜像仍能开机

无修改（A 面）
[ ] 冷启动后 boss systemless verify 返回 0（改动只来自 /data）
[ ] /system 的 hash 与刷机前一致
[ ] 只读分区仍为 ro（verify 第 ② 项）
[ ] 装一个带 system/ 的模块后再次 verify，仍应判"干净"（差异不定罪，见 2.1）
[ ] systemless.conf 的属性在 post-fs-data 生效，且不触发 on property: 事件

特典（B 面）
[ ] boss hide mounts 列出的条数与模块挂载情况对得上
[ ] 名单加入某 app 后，该 app 视野内看不到 BOSS 挂载（cat /proc/<pid>/mountinfo）
[ ] 同 ns 的其它进程也一并生效（这是 3.2 的已知边界，确认它符合预期）
[ ] 目标 app 重启后能被再次处理（去重记录不能让它漏掉）
[ ] boss ping 仍返回 up（隐藏不该搞挂 daemon）

隐蔽性回归（每批都要，沿用任务4 第 8 节）
[ ] 没有引入新的可被检测的属性
[ ] /system 的 hash 与刷机前一致
```

---

## 8. 已知风险与没验的部分

诚实列出来，避免下一位把"没验过"当成"验过"：

| 项 | 状态 | 说明 |
|---|---|---|
| **SwitchRoot 劫持的实际效果** | **未上真机** | 代码已补齐、dry run 与变砖保护已验；切根后的落点沙盒验不到（无内核、无 SwitchRoot） |
| **post-fs-data 触发者的时序** | **未上真机** | 轮询触发比 init 原生 trigger 晚，取决于 /data 解密耗时，需真机量 |
| 隐藏的实际效果 | **未上真机** | ns 共享限制（3.2）决定了它的效果范围，需在真机 app 上确认 |
| `boss systemless verify` 的①项 | 需真实分区布局 | 沙盒无 `/system`，脚本明确标 SKIP |
| 属性伪装的持久性 | 依赖机型 | 动态属性会被系统服务改回；`persist.*` 需配合 `-p` |
| hexpatch 退路 | 大概率永不触发 | `/init` 正在执行时 `open(O_RDWR)` 会 ETXTBSY，它本是**离线 patch 镜像**用的手段，运行时基本做不到 |

**沙盒里已经验到的**（7 套，共 136 项，含任务5 新增 42 项）：

| 套件 | 项数 | 覆盖 |
|---|---|---|
| `smoke_test.sh` | 28 | 原有主链路回归 |
| `component_test.sh` | 36 | 原有任务3 边界回归 |
| `selinux_test.sh` | 24 | 原有任务4 回归 |
| `stage2_test.sh` | 6 | 原有 init 接线回归 |
| `systemless_test.sh` | 16 | 清单解析容错、dry run 无副作用、属性端到端、基线比对、返回码 |
| `hide_test.sh` | 17 | 名单增删幂等、暴露面清单、摘挂载、守护 `--once` 必退出、模板边界说明 |
| `hijack_test.sh` | 9 | dry run 无副作用、非 root 不拖垮开机、变砖保护、阶段参数不丢 |

---

## 9. 上手路径

```bash
make test

# A 面
./build/boss systemless status
./build/boss systemless plan                      # 只看打算做什么
./build/boss systemless apply --stage post-fs-data
./build/boss systemless verify                    # 零写入自检
./build/boss systemless verify --save             # 刷机后先存一份基线

# B 面
./build/boss hide mounts                          # 先看暴露面
./build/boss hide denylist add com.example.app
./build/boss hide umount <pid> --dry
./build/boss hide daemon --once
./build/boss hide props --save-template

# 前置缺口（真机 bring-up 第一件事）
./build/boss init hijack-prep --dry
ls -l /proc/1/exe                                 # 真机判定劫持有没有成
```

新增/改动的文件：

```
src/mntinfo.c              挂载表解析（verify 与 hide 共用，只有这一份）
src/systemless.c           A 面：清单引擎 + 零写入自检
src/hide.c                 B 面：隐藏名单 / 摘挂载 / 守护 / 属性伪装
src/bossinit.c             补完 hijack-prep（mount + /init.real）+ 开机触发者
src/boot.c                 编排加入 systemless 与挂载快照；service 阶段起隐藏守护
src/applet.c               注册 systemless / hide
src/util.c                 boss_copy 自拷贝保护（踩过的坑）
src/boss.h                 任务5 路径与入口声明
payload/init.boss.rc       on early-init 触发 hijack-prep（切根前）
policy/systemless.conf.example  清单模板
tools/systemless_test.sh   16 项验收
tools/hide_test.sh         17 项验收
tools/hijack_test.sh       9 项验收
.github/workflows/task5.yml  任务5 CI + 挂载解析单一来源自检
```
