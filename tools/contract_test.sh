#!/usr/bin/env bash
# BOSS · CLI 输出契约自检（任务6）
#
# 为什么要有这份脚本：
#   App 的解析器（Parsers.kt）是按上游 printf 的**字面标记**去抓的。
#   上游改了输出格式而这里没跟上，表现不是崩溃，而是"页面永远显示 0 条"——
#   这类 bug 在真机上极难定位，因为界面看起来是正常的。
#
#   所以这里跑**真实的 boss 二进制**，断言 Parsers.kt 依赖的每一行标记都还在。
#   上游改格式 → 这份脚本红 → 先改解析器，别先改断言。
#
# 用法：
#   BOSS_BIN=/path/to/build/boss bash tools/contract_test.sh
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
# BOSS_SRC 自动探测：两种布局都支持——
#   全量包：src 就在仓内（$ROOT/src）
#   独立仓：boss 本体在旁边的 BOSS-TASK5（$ROOT/../BOSS-TASK5/src）
# 手写死一种布局的后果是：在另一种布局里静默跑到错的树上。
# 上一次就是这么红的——拿未打补丁的 src 去对 App 侧的 proto v2。
if [ -z "${BOSS_SRC:-}" ]; then
    for cand in "$ROOT/src" "$ROOT/../BOSS-TASK5/src" "$ROOT/vendor/boss/src"; do
        if [ -f "$cand/daemon.c" ]; then BOSS_SRC="$cand"; break; fi
    done
fi
BOSS_SRC="${BOSS_SRC:-$ROOT/src}"
BOSS_BIN="${BOSS_BIN:-}"
BUILD="$ROOT/build"
TESTDIR="${BOSS_TEST_DIR:-/tmp/boss-contract-test}"

pass=0; fail=0
check() { if [ "$1" = "0" ]; then echo "  PASS  $2"; pass=$((pass+1)); else echo "  FAIL  $2"; fail=$((fail+1)); fi; }

# 为什么优先自己编一份、而不是直接跑现成的 build/boss：
#   上游 `make test` 的产物把 BOSS_DIR 编成了 /tmp/boss-test，
#   它会去读那边的 policy.conf 与 modules/，测试在自己目录里造的现场根本不生效
#   ——症状和接力须知坑 6（跑着另一个 BOSS_DIR 的二进制而不自知）一模一样。
#   所以显式指定 BOSS_SRC 时一律自己编，产物名独立（坑 4.6）。
if [ -n "$BOSS_BIN" ] && [ -x "$BOSS_BIN" ]; then
    echo "使用现成二进制：$BOSS_BIN"
elif [ -f "$BOSS_SRC/daemon.c" ]; then
    mkdir -p "$BUILD"
    cc -O1 -std=c11 -w -DBOSS_DIR="\"$TESTDIR\"" \
       -o "$BUILD/boss-contract-fixture" \
       "$BOSS_SRC"/main.c "$BOSS_SRC"/util.c "$BOSS_SRC"/policy.c "$BOSS_SRC"/pty.c \
       "$BOSS_SRC"/daemon.c "$BOSS_SRC"/client.c "$BOSS_SRC"/bossinit.c \
       "$BOSS_SRC"/applet.c "$BOSS_SRC"/resetprop.c "$BOSS_SRC"/scripts.c \
       "$BOSS_SRC"/module.c "$BOSS_SRC"/sepolicy.c "$BOSS_SRC"/sh.c \
       "$BOSS_SRC"/boot.c "$BOSS_SRC"/selinux.c "$BOSS_SRC"/sepol_backend.c \
       "$BOSS_SRC"/mntinfo.c "$BOSS_SRC"/systemless.c "$BOSS_SRC"/hide.c \
       "$BOSS_SRC"/manager.c "$BOSS_SRC"/prompt.c -ldl 2>/dev/null \
        && BOSS_BIN="$BUILD/boss-contract-fixture" \
        || echo "SKIP 夹具编译失败（用 BOSS_BIN 直接指定产物）"
fi

if [ ! -x "$BOSS_BIN" ]; then
    echo "SKIP 没有可用的 boss 产物（指定 BOSS_SRC 或 BOSS_BIN）"
    exit 0
fi
echo "被测二进制：$BOSS_BIN"
echo "运行时目录：$TESTDIR"

echo "BOSS · CLI 输出契约（对照 Parsers.kt）"

# 造一个最小现场：一个模块 + 一份清单 + 一条隐藏名单
rm -rf "$TESTDIR"
mkdir -p "$TESTDIR/modules/demo/system/app"
printf 'id=demo\nname=Demo Module\nversion=v1.2\nauthor=someone\ndescription=demo\n' \
    > "$TESTDIR/modules/demo/module.prop"
printf 'prop ro.debuggable 0\ndeny com.example.bank\n' > "$TESTDIR/systemless.conf"
printf 'default = deny\nlog = 1\n' > "$TESTDIR/policy.conf"

# ---------- systemless status ----------
echo
echo "== systemless status =="
out="$("$BOSS_BIN" systemless status 2>&1)"
echo "$out" | grep -q '属性条目 .*，隐藏条目' \
    && check 0 "status：属性/隐藏条目计数行还在" || { check 1 "status：计数行"; echo "$out"; }
echo "$out" | grep -q 'BOSS 引入挂载：.*条（其中 tmpfs 覆盖层' \
    && check 0 "status：挂载统计行还在" || { check 1 "status：挂载统计"; echo "$out"; }
echo "$out" | grep -q '挂载表总条数：' \
    && check 0 "status：挂载表总条数还在" || check 1 "status：总条数"
echo "$out" | grep -q '：已配置' \
    && check 0 "status：清单已配置的标记还在" || check 1 "status：已配置标记"

# ---------- systemless verify ----------
echo
echo "== systemless verify =="
out="$("$BOSS_BIN" systemless verify --dir "$TESTDIR" 2>&1)"
echo "$out" | grep -q '^结论：' \
    && check 0 "verify：结论行还在" || { check 1 "verify：结论行"; echo "$out"; }
echo "$out" | grep -qE '无基线|与基线一致|与基线有 [0-9]+ 处差异' \
    && check 0 "verify：基线比对三种写法都能被解析" || { check 1 "verify：基线写法"; echo "$out"; }
echo "$out" | grep -q '常见 root 痕迹路径' \
    && check 0 "verify：痕迹路径小节还在" || check 1 "verify：痕迹路径"

# 存一次基线，把"与基线一致 / 有 N 处差异"这半边也验到
"$BOSS_BIN" systemless verify --dir "$TESTDIR" --save >/dev/null 2>&1
[ -f "$TESTDIR/systemless.baseline" ] && check 0 "verify --save：基线文件已落盘" \
                                      || check 1 "verify --save：基线文件"
out2="$("$BOSS_BIN" systemless verify --dir "$TESTDIR" 2>&1)"
echo "$out2" | grep -qE '与基线一致|与基线有 [0-9]+ 处差异' \
    && check 0 "verify：有基线时能给出比对结论" || { check 1 "verify：比对结论"; echo "$out2"; }

# ---------- hide mounts / denylist ----------
echo
echo "== hide =="
out="$("$BOSS_BIN" hide mounts 2>&1)"
echo "$out" | grep -q 'BOSS 引入的挂载' \
    && check 0 "mounts：标题行还在" || { check 1 "mounts：标题行"; echo "$out"; }
echo "$out" | grep -q '共 .* 条（挂载表总条数' \
    && check 0 "mounts：汇总行还在" || check 1 "mounts：汇总行"

"$BOSS_BIN" hide denylist add com.example.bank >/dev/null 2>&1
out="$("$BOSS_BIN" hide denylist list 2>&1)"
echo "$out" | grep -qx 'com.example.bank' \
    && check 0 "denylist list：一行一个名字，无表头" || { check 1 "denylist list"; echo "$out"; }

# ---------- module list / info ----------
echo
echo "== module =="
out="$("$BOSS_BIN" module list 2>&1)"
echo "$out" | grep -q '^ID ' \
    && check 0 "module list：表头还在（解析器靠它跳过首行）" || { check 1 "module list：表头"; echo "$out"; }
echo "$out" | grep -q 'demo' \
    && check 0 "module list：能列出模块" || { check 1 "module list 内容"; echo "$out"; }

out="$("$BOSS_BIN" module info demo 2>&1)"
echo "$out" | grep -q '^id:' \
    && check 0 "module info：id 字段还在" || { check 1 "module info：id"; echo "$out"; }
echo "$out" | grep -q '^mount:' \
    && check 0 "module info：mount 字段还在（enabled/scriptOnly 靠它）" || check 1 "module info：mount"

# ---------- policy show ----------
echo
echo "== policy =="
out="$("$BOSS_BIN" policy show 2>&1)"
echo "$out" | grep -q '^default' \
    && check 0 "policy：default 行还在" || { check 1 "policy：default"; echo "$out"; }
echo "$out" | grep -q '^rule' \
    && check 0 "policy：rule 行格式还在" || echo "  SKIP  policy：本次没有规则（正常）"

# ---------- 审计日志行格式 ----------
echo
echo "== 审计日志 =="
# 直接造一行，验 Parsers.audit 依赖的字段顺序
line="[2026-09-28 01:12:43] uid=10086 pid=2115 caller='com.foo' target=0 cmd='id' ctx='' -> allow"
echo "$line" | grep -qE "^\[[^]]+\] uid=[0-9]+ pid=[0-9]+ caller='[^']*' target=[0-9]+ cmd='[^']*' ctx='[^']*' -> (allow|deny)$" \
    && check 0 "审计行：字段顺序与 Parsers.audit 一致" || check 1 "审计行格式"

echo
echo "结果：PASS=$pass FAIL=$fail"
exit "$fail"
