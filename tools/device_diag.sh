#!/bin/bash
# BOSS · init 接管真机诊断
#
# 为什么要有这个脚本：
#   init 接管是"出错就是变砖"的链路，而沙盒里没有内核、没有 SwitchRoot 可观察，
#   离机测试最多验到"命令拼对了"。真机上一旦不生效，症状是
#   "代码全绿、界面全灰、没有任何报错"——必须靠现场状态定位断在哪一环。
#
# 用法（二选一）：
#   A. PC 上跑（推荐，有 adb）：      bash tools/device_diag.sh
#   B. 推到手机跑：adb push tools/device_diag.sh /data/local/tmp/ && adb shell sh /data/local/tmp/device_diag.sh
#
# 把输出整段贴回来即可。脚本不改动任何东西，只读。
set -u

say() { printf '%s\n' "$*"; }
hr()  { say "------------------------------------------------------------"; }

# 判断是在 PC（有 adb）还是在手机上（无 adb）
if command -v adb >/dev/null 2>&1 && adb shell 'true' >/dev/null 2>&1; then
    RUN() { adb shell "$@" 2>&1; }
    say "== 模式：PC（adb）=="
    say "  设备：$(adb shell getprop ro.product.model 2>/dev/null) / $(adb shell getprop ro.build.version.release 2>/dev/null)"
else
    RUN() { eval "$@" 2>&1; }
    say "== 模式：手机本地（无 adb）=="
fi

hr
say "== 1) 谁在当 init（**最关键**，其余全部依赖它）=="
say "  /proc/1/exe -> $(RUN 'readlink -f /proc/1/exe 2>/dev/null || ls -l /proc/1/exe 2>/dev/null')"
say "  /proc/1/cmdline: $(RUN 'tr "\0" " " < /proc/1/cmdline 2>/dev/null')"
say ""
say "  判定："
RUN 'readlink /proc/1/exe 2>/dev/null' | grep -qi boss \
    && say "    ✓ 指向 BOSS —— 劫持成功" \
    || say "    ✗ 不指向 BOSS —— 劫持没生效（下面逐环排查）"

hr
say "== 2) payload 到位了吗（veritpath 注入的结果）=="
say "  /boss           : $(RUN 'ls -l /boss 2>/dev/null || echo 不存在')"
say "  /init.boss.rc   : $(RUN 'ls -l /init.boss.rc 2>/dev/null || echo 不存在')"
say "  /init.rc 里的 import:"
RUN 'grep -n "boss" /init.rc 2>/dev/null || echo "    （/init.rc 里没有 boss 字样 —— rc 没被 import）"'

hr
say "== 3) hijack-prep 做过吗（切根前的布置）=="
say "  /init.real      : $(RUN 'ls -l /init.real 2>/dev/null || echo 不存在')"
say "  /init           : $(RUN 'ls -l /init 2>/dev/null || echo 不存在')"
say "  /storage/self/primary : $(RUN 'ls -l /storage/self/primary 2>/dev/null || echo 不存在')"
say ""
say "  /sdcard 的挂载（hijack-prep 把 boss bind 到这里）："
RUN 'grep -E " /sdcard | /storage/self/primary " /proc/self/mountinfo 2>/dev/null || echo "    （mountinfo 里没有 /sdcard —— bind 没成功或已被 SwitchRoot 移走）"'

hr
say "== 4) 内核日志里 BOSS 说了什么=="
RUN 'dmesg 2>/dev/null | grep -i boss | tail -30 || echo "    （dmesg 无 boss 日志：可能 early-init 没执行，或 kmsg 被过滤）"'

hr
say "== 5) 布局判断（决定劫持该走哪条路）=="
say "  ro.boot.slot_suffix   : $(RUN 'getprop ro.boot.slot_suffix')"
say "  init_boot 分区        : $(RUN 'ls -l /dev/block/by-name/init_boot_a 2>/dev/null || ls -l /dev/block/by-name/init_boot 2>/dev/null || echo 无独立 init_boot')"
say "  /system 是否独立分区  : $(RUN 'grep -c " /system " /proc/self/mountinfo 2>/dev/null')"
say "  Android 版本          : $(RUN 'getprop ro.build.version.release')  API $(RUN 'getprop ro.build.version.sdk')"
say ""
say "  提示：Android 13+ GKI 的多数设备是 2SI（ramdisk 在 init_boot，"
say "        第一阶段结束 SwitchRoot）。这类设备 **必须** 走 hijack-prep；"
say "        init.boss.rc 里 post-fs-data 之后的条目在切根后根本看不到该文件。"

hr
say "== 6) bossd 与协议（接管的下游现象）=="
say "  bossd 进程 : $(RUN 'ps -A 2>/dev/null | grep -i boss || echo 未运行')"
say "  boss -V    : $(RUN '/boss -V 2>&1 || echo "（跑不起来）"')"
say "  SELinux    : $(RUN 'getenforce 2>/dev/null || echo unknown')"

hr
say "== 7) 手工复核（可选，Root 类终端里执行）=="
say "  ls -l /proc/1/exe"
say "  /boss init hijack-prep --dry     # 只打印将做什么，不动系统"
say "  dmesg | grep -i boss"
hr
say "诊断结束。把上面整段输出贴回来即可定位断在哪一环。"
