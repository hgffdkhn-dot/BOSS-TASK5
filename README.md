# BOSS · 全量包（任务 2 / 3 / 4 / 5 / 6）

> **这是一个全量包**：BOSS-TASK5 的全部内容 + 任务6 的增量。
>
> ⚠️ **本仓【不含】计划表第 1 项的 veritpath。**
> 它是 1 号的独立仓库 `hgffdkhn-dot/veritpath.git`，不在这里，也不需要在这里——
> 两者的关系是"输入 / 工具"而不是"包含"：
>   本仓**产出** `build/payload`（manifest.json + init.boss.rc + boss 二进制），
>   veritpath **消费**这个 payload 去分析 boot 并注入镜像。
> CI 的 payload job 会现取现编（`git clone --depth 1 $VP_REPO`，地址见
> `.github/workflows/ci.yml` 的 `VP_REPO`，可用 repo variable 换源），
> 然后跑 payload-check → inject → verify → unpack 一整条。
> **交付给第三方时，veritpath 要单独给一份。**
> （另：`payload/manifest.json` 里那个 `/veritpath/boss` 是镜像内的**冗余放置路径**，
>  `required: false`，与工具本身无关，别被同名误导。）
> 前辈的文件一份都没删，只做增量添加与修改。两种用法：
>   · 覆盖式推到 `BOSS-TASK5`（推荐，单一事实来源）
>   · 或推到新仓 `BOSS-TASK6`（前辈存档原样保留）
> 改 `PUSH-TO-GITHUB.sh` 里的 `REPO_NAME` 即可，或在命令行覆盖：
> `REPO_NAME=BOSS-TASK6 bash PUSH-TO-GITHUB.sh`



Magisk 式 root 管理器 BOSS 的 su 子系统与关键组件：主打**隐蔽、日用、实用**。
镜像注入交给上游的 veritpath。

- **任务6（客户端）的交付说明**：`docs/TASK6-BOSS客户端-解析与交付.md`
- **任务6（客户端）接手先看**：`docs/HANDOFF-TASK6-客户端.md`（操作手册：
  三条硬约束、接口契约、别做错的事）
- 任务5 的**交付说明**：`docs/TASK5-无修改系统逻辑与特典逻辑.md`（设计推演）
- 方案解析与设计说明：`docs/BOSS-su-方案解析与设计.md`
- **接力开发者先看**：`docs/HANDOFF-接力须知.md`（踩过的坑、架构红线、v0.2 实施手册）
- 任务3（关键组件）的**任务书**：`docs/HANDOFF-TASK3-关键组件.md`
- 任务3 的**交付说明**：`docs/TASK3-组件交付与接力.md`（已落地什么、怎么验、给下游的接口）
- 给任务4 的 SELinux 权限清单：`docs/SELINUX-REQUIREMENTS.md`
- 任务4 的**交付说明**：`docs/TASK4-SELinux-解决与规则注入.md`
- 任务5 的**交付说明**：`docs/TASK5-无修改系统逻辑与特典逻辑.md`（systemless / hide / hijack 补完）
- 注入契约与真机流程：见文档第 8 节

> ⚠️ **交接时的三条硬约束**（任务5 结束时仍然成立，详见 `docs/HANDOFF-TASK6-客户端.md`）：
> 1. v0.2 的 init 接管**代码已补齐但只能真机验**。接手一台没验过的机器，
>    第一件事是 `ls -l /proc/1/exe`，**不是写代码**。
> 2. 改任何测试套件后，**root 与非 root 两种身份都要跑一遍**再提交——
>    CI 的 runner 是非 root，只跑 root 会得出错误结论（这条真红过一次）。
> 3. 仓库可能还没推上去：本地已 commit + 打 `v0.2.0` tag，
>    建空仓与 push 需主办方执行 `bash PUSH-TO-GITHUB.sh`。

## 目录

```
app/        BOSS 客户端（Kotlin + 一个很小的 JNI 层）—— 任务6
            src/main/cpp/  协议客户端 + JNI 胶水
            src/main/java/ core（协议/CLI 契约）data（解析/仓库）ui（四个页面 + M3 Expressive）
src/        boss.h / main.c / daemon.c / client.c / policy.c / pty.c / util.c / bossinit.c
            manager.c（任务6：App 身份识别）/ prompt.c（任务6：授权弹窗）
            applet.c（分发）/ resetprop.c（属性改写）/ module.c（模块挂载）
            scripts.c（boot 脚本）/ boot.c（开机编排）/ sepolicy.c（策略工具）/ sh.c（工具集）
            mntinfo.c（挂载表解析）/ systemless.c（无修改系统逻辑）/ hide.c（特典）
patches/    任务6 给 daemon 侧的补丁（已合入本包，留档备查）
payload/    manifest.json + init.boss.rc（veritpath payload）
build/      build-ndk.sh（NDK 交叉编译 + 组装 payload）
tools/      smoke_test.sh（冒烟测试）、mkprop.py（造合成属性区）、elf_fix.py（修 PT_TLS 对齐）
.github/    ci.yml / release.yml
docs/       方案解析与设计、HANDOFF 接力须知、TASK3 交付说明、SELINUX 需求清单
```

## CI

两组工作流：

- `ci.yml`（4 个 job）：冒烟端到端（28 项）、多 `-std` `-Werror` 严格编译、
  payload 用 veritpath 实际注入校验、Android 四 ABI 交叉编译 + Bionic 加载校验
- `components.yml`（2 个 job）：任务3 组件的**深度验收** + 属性区布局断言自检
- `task5.yml`（2 个 job）：任务5 三套验收（systemless 16 / hide 17 / hijack 9）
  + 挂载解析"只有一份"的自检
- `task6.yml`（5 个 job）：协议布局断言 / IPC 端到端 / manager+弹窗 / CLI 契约
  + 上游回归（确认任务6 没碰坏 2/3/4/5）

```bash
bash tools/smoke_test.sh        # 端到端主链路，28 项
bash tools/component_test.sh    # 任务3 边界与语义细节（root 与非 root 都过）
bash tools/systemless_test.sh   # 任务5 A 面：无修改系统逻辑
bash tools/hide_test.sh         # 任务5 B 面：特典逻辑
bash tools/hijack_test.sh       # 任务5 补完的 SwitchRoot 劫持布置

bash tools/run_ipc_test.sh      # 任务6：协议布局断言 + IPC 端到端（两种身份）
bash tools/manager_flow_test.sh # 任务6：manager 身份 + 授权弹窗（14 项）
bash tools/contract_test.sh     # 任务6：CLI 输出契约（App 解析器的护栏，18 项）
```

**编 APK 之前先跑**（不需要 Android SDK）：

```bash
bash tools/check_android_toolchain.sh
```

它会把 AGP / Kotlin / BOM / material3 / compileSdk / targetSdk 打出来并对着下限校验。
AAR 元数据校验失败时会甩出二三十条 "requires AGP 9.1.0 / requires compileSdk 37"，
而真实原因往往只是版本链上某一节没跟上——这个脚本直接指出是哪一节。

```bash
git tag v0.1.0 && git push origin v0.1.0    # 打 tag 自动出 Release（含 payload zip）
```

## 快速开始（主机验证，无需真机）

```bash
make test                     # 编译，运行时目录指向 /tmp/boss-test
bash tools/smoke_test.sh      # 28 项冒烟测试，应全部 PASS（root 与非 root 都过）
make static                   # 静态 PIE 产物 build/boss-static
./build/boss daemon           # 起 daemon（后台）
./build/boss su -c id         # 走一遍授权链路
./build/boss policy show      # 看策略
```

## 交叉编译（Android）

```bash
bash build/build-ndk.sh                  # -> dist/boss-android-{arm64,arm,x86_64,x86}
MAKE_PAYLOAD=1 bash build/build-ndk.sh   # 顺带生成 build/payload/ 与 dist/boss-payload.zip
WITH_SEPOL=1 bash build/build-ndk.sh     # 内置 libsepol（早期注入需要，先跑 vendor-sepol.sh）
```

> ⚠️ 这个脚本在 `build/` 下，而 `build/` 同时是构建产物目录。
> **别把 `build/` 整目录写进 `.gitignore`**——脚本会永远提交不上去，
> CI 第一步 `bash build/build-ndk.sh` 就 `No such file or directory`，
> 而本地一直是绿的（本地工作区里那个文件好端端躺着）。
> 这个坑真实发生过。现在的处理是 `build/*` + `!build/build-ndk.sh`。

`build-ndk.sh` 不赌某一个链接标志组合：`-static -fPIE -pie` 并**不会**得到
静态 PIE（链接器见到 `-static` 就关掉 PIE，产出 `ET_EXEC`）。它按候选顺序试
`-static-pie` → `-static -fPIE -pie` → `-static -fPIE -pie -Wl,-pie` → `-fPIE -pie`，
每个候选都先 `tools/elf_fix.py` 修 PT_TLS 对齐再 `--check` 验收，取第一个产出
`ET_DYN` 的。编译通过 ≠ 上机能跑，这道校验不能删。

## 注入（真机）

```bash
veritpath payload-check build/payload
veritpath inject --init-boot init_boot.img -p build/payload -o out/
veritpath verify out/init_boot.veritpath.img -p build/payload   # 非零退出就别刷
fastboot flash init_boot out/init_boot.veritpath.img
```

## 任务6 · 客户端

```bash
./gradlew :app:assembleDebug
adb install app/build/outputs/apk/debug/app-debug.apk
```

- 主题是 **Material 3 Expressive**（Android 16 原生视觉语言）：动态取色 + 弹簧动效。
  ⚠️ `material3` 必须锁死在 `1.5.0-alpha27`，**别让 Compose BOM 覆盖掉它**——
  BOM 会把它拉回 1.4.0 稳定版，于是 Expressive 那几个 API 全部 unresolved，
  而报错不会提示"版本被 BOM 换掉了"。
- `applicationId` 必须与 daemon 侧的 `BOSS_MANAGER_PKG` 一致（`com.boss.manager`）。
  包名对不上时 manager 判定永远失败，表现是"所有功能都显示被拒绝"且不报错。
- App **不申请任何权限**。多一个权限就是多一份痕迹。

## 运行时布局

```
/data/adb/boss/boss                 bossd 与 su 本体（单二进制多入口）
/data/adb/boss/policy.conf          授权策略
/data/adb/boss/boss.log             日志（可关）
/data/adb/boss/bin/                 busybox 与 applet symlink（App 释放，不进 ramdisk）
/data/adb/boss/modules/<id>/        模块（module.prop / system / system.prop / sepolicy.rule …）
/data/adb/boss/systemless.conf     任务5：声明式"系统改动"清单（单一事实来源）
/data/adb/boss/systemless.baseline 任务5：只读分区基线快照（verify 用）
/data/adb/boss/denylist.conf       任务5：隐藏名单
/data/adb/boss/hide.state          任务5：已处理的 (pid, starttime)，防重复
/data/adb/boss/props.conf          任务5：属性伪装清单
/data/adb/boss/manager.uid         任务6：BOSS App 的 uid（daemon 侧认定）
/data/adb/boss/prompt/<id>.req     任务6：待用户裁决的授权请求
/data/adb/boss/prompt/<id>.ans     任务6：裁决结果（allow / deny）
/data/adb/boss/post-fs-data.d/      阻塞阶段脚本
/data/adb/boss/service.d/           late_start 脚本（绝大多数脚本的默认选择）
/data/adb/boss/boot-completed.d/    开机完成后的脚本
/data/adb/boss/persist.props        persist 属性快照（每次开机重放）
@bossd                              AF_UNIX 抽象套接字（文件系统无节点）
```

## 关键组件常用命令

```bash
boss --list                         列出所有组件
boss resetprop ro.debuggable 1      改只读属性（默认走 property_service，会触发 rc 事件）
boss resetprop -n ro.foo bar        直写属性区，不触发事件（post-fs-data 阶段必用）
boss resetprop --delete ro.foo      删除属性
boss resetprop -p --delete persist.foo   删除 persist 属性且重启后不恢复
boss module list | plan | mount     模块列表 / 挂载计划（dry run）/ 真挂载
boss module dump props|rules        汇总模块的 system.prop / sepolicy.rule
boss script post-fs-data|service|boot-completed   按阶段跑脚本
boss boot post-fs-data|service|completed          开机编排（顺序已固定，rc 只调它）
boss sh                             standalone busybox shell（脚本兼容性）
boss sepolicy check|apply|info      SELinux 规则规范化 / 应用 / 定位策略文件
```

## 任务5 常用命令

```bash
boss systemless apply|plan|verify|status   无修改系统逻辑：清单应用 / 零写入自检
boss hide denylist add|del|list            隐藏名单
boss hide mounts                           列出 BOSS 引入的挂载（暴露面）
boss hide umount <pid>                     在目标进程视野里摘除挂载
boss hide daemon --once                    扫描并处理命中名单的进程
boss hide props                            属性伪装清单（systemless 语法）
boss init hijack-prep --dry                SwitchRoot 劫持布置（离线演练）
```

## 常用命令

```bash
boss su -c 'id'                 请求 root
boss su -s /system/bin/sh       指定 shell
boss su -u 0 -g 0 -c 'cmd'      指定目标 uid/gid
boss policy add uid 2000 allow  允许 adb shell
boss policy set default deny    默认拒绝
boss policy set log 0           关日志（日用）
boss init install | start       落地/拉起（供 rc 调用）
boss ping                       探活
```

## 当前状态（对应计划表）

| 计划项 | 状态 |
|---|---|
| 1. boot 分析与注入工具 | ✅ 上游 veritpath 已完成 |
| 2. su 打造 | ✅ v0.1 已交付；v0.2（2SI init 接管）待做 |
| 3. 其他关键文件与重要组件 | ✅ 已交付（resetprop / 模块挂载 / boot 脚本 / sepolicy 工具 / 工具集 / 开机编排） |
| 4. SELinux 解决 | ✅ 已交付（255 条规则 + 引擎 + 早期注入接线，未上真机） |
| 5. 无修改系统逻辑与特典逻辑 | ✅ 已交付（systemless / hide / hijack 补完，部分需真机验） |
| 6. BOSS 客户端与对接修补 | ✅ 已交付（App + manager 身份 + 授权弹窗；真机待验）|
| 7. 长期开发 | ⬜ |

已知最大限制：v0.2 的 init 接管（SwitchRoot 劫持）**代码已补齐但只能在真机验**。
沙盒里能验到"命令拼对了、dry run 无副作用、变砖保护还在"，验不到"切根后真的
落到 `/system/bin/init`"。上机第一件事：`ls -l /proc/1/exe` 指向我们自己就说明
劫持成功；指向原厂 init 就没成。务必带上 `boss_selinux=0` 自救路径再上机。
详见 `docs/TASK5-无修改系统逻辑与特典逻辑.md`。
