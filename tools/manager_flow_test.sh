#!/usr/bin/env bash
# BOSS · 任务6：manager 自动放行 + 授权弹窗 端到端
#
# 验的是任务6 接手时那两格空的：
#   "BOSS 前端授权 ❌ 没做"  → manager 自动放行（鸡生蛋问题的解法）
#   "prompt 按拒绝处理"      → 真的等用户点一下
#
# 关键手法：用 `exec -a com.boss.manager` 把进程伪装成 App。
# 真机上 cmdline 是 zygote 填的包名，沙盒里只能自己演——
# 但它验的判定逻辑（内核 uid + cmdline 双锁）与真机完全一致。
#
# ⚠️ 非 root 与 root 两种身份都要能跑；CI 是非 root，以那一份为准。
#    非 root 下 daemon 会把目标身份降级为自身（daemon.c 有这条分支），
#    授权链路本身照样验得到。
#
# 用法：BOSS_SRC=/path/to/patched-boss/src bash tools/manager_flow_test.sh
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
BUILD="${BOSS_BUILD_DIR:-$ROOT/build}"   # 可覆盖，方便在受限环境里定位问题
TESTDIR="${BOSS_TEST_DIR:-/tmp/boss-task6-test}"
PROBE="$BUILD/ipc_probe"

fails=0
check() { if [ "$1" = "0" ]; then echo "  PASS  $2"; else echo "  FAIL  $2"; fails=$((fails + 1)); fi; }

if [ ! -f "$BOSS_SRC/daemon.c" ]; then
    echo "SKIP 找不到上游 src（用 BOSS_SRC 指定，必须是打过任务6 补丁的树）"
    exit 0
fi
if [ ! -f "$BOSS_SRC/manager.c" ] || [ ! -f "$BOSS_SRC/prompt.c" ]; then
    echo "SKIP $BOSS_SRC 里没有 manager.c / prompt.c —— 任务6 的 daemon 侧补丁没合进去。"
    echo "     先：git apply patches/0001-task6-manager-and-prompt.patch"
    exit 0
fi

# ---- 身份能力探测（CI 上就是倒在这一步）----
# 本套件必须能**切 uid**：manager 判定要求 uid>=10000 才算"app"，
# 还要有第二个 uid 去冒充它。切 uid 是特权能力——非 root 且没有免密 sudo 的
# 机器上根本做不到（setpriv: setresuid failed: Operation not permitted）。
#
# 那种情况下必须**老老实实 SKIP**，不能报一堆 FAIL：
# 9 个 FAIL 会让人以为 manager 自动放行、弹窗全都坏了，
# 而实际上一个用例都没真正跑起来。能力不具备 ≠ 代码有问题。
if [ "$(id -u)" != "0" ]; then
    if [ "${BOSS_SUDO_ATTEMPTED:-0}" = "1" ]; then
        # 提过一次还是非 root：sudo 存在但没真给到 root（容器里常见）。
        # 没有这个闸门就是无限重入——exec 自杀式循环，CI 上表现为超时。
        echo "SKIP 已尝试提权但当前仍是 uid=$(id -u)：sudo 没真正生效，放弃。"
        exit 0
    fi
    if sudo -n true 2>/dev/null; then
        echo "[i] 非 root 但有免密 sudo —— 整份脚本重新以 root 执行"
        echo "    （真机上 daemon 本来就是 root，这一份才是高保真那一份）"
        exec sudo -n env BOSS_SUDO_ATTEMPTED=1 \
             BOSS_SRC="$BOSS_SRC" BOSS_TEST_DIR="$TESTDIR" bash "$0"
    fi
    echo "SKIP 本套件需要切换 uid（manager 判定要求 uid>=10000），当前非 root 且没有免密 sudo。"
    echo "     能力不具备，不是代码有问题。本地请以 root 跑：sudo bash $0"
    exit 0
fi

MGR_UID=65534     # BOSS App
ASK_UID=65533     # 策略 = prompt（弹窗用例）
DENY_UID=65532    # 无规则 → default=deny，立即返回
FAKE_UID=65531    # 想冒充 manager 的第三者

echo "=== 0) 选一个能跑二进制的目录，再编夹具与探针 ==="

# 为什么"先选目录、后编译"：
#   容器化 runner 的工作区可能挂了 noexec。那种情况下**所有**产物都跑不起来，
#   而且 root 也一样跑不起来（noexec 不看 uid）。
#   如果先编 daemon 夹具、后才发现跑不了，就会得到"daemon 没了"这种
#   离真实原因十万八千里的报错。所以先用一个小探针试出能 exec 的目录。
probe_boot() {   # $1=uid
    chmod 0755 "$BUILD" 2>/dev/null
    chmod 0755 "$PROBE" 2>/dev/null
    setpriv --reuid="$1" --regid="$1" --clear-groups \
        env HOME=/tmp bash --noprofile --norc -c "exec -a com.boss.manager $PROBE selftest" >/dev/null 2>&1
}

diagnose() {     # $1=uid
    echo "     诊断（uid=$1 无法 exec 探针）："
    echo "       · 探针      : $PROBE"
    ls -l "$PROBE" 2>&1 | sed 's/^/         /'
    ls -ld "$BUILD" 2>&1 | sed 's/^/       · build 目录: /'
    echo "       · umask    : $(umask)"
    echo "       · 挂载选项 : $(findmnt -no OPTIONS -T "$BUILD" 2>/dev/null || echo '?')"
    setpriv --reuid="$1" --regid="$1" --clear-groups \
        env HOME=/tmp bash --noprofile --norc -c "exec -a com.boss.manager $PROBE selftest" 2>&1 \
        | sed 's/^/       · 实际报错: /'
}

try_dir() {      # $1=候选目录；成功返回 0
    BUILD="$1"
    PROBE="$BUILD/ipc_probe"
    mkdir -p "$BUILD" 2>/dev/null || return 1
    cc -O1 -std=c11 -w -I"$ROOT/app/src/main/cpp" -o "$PROBE" \
       "$HERE/ipc_probe.c" "$ROOT/app/src/main/cpp/boss_ipc.c" 2>/dev/null || return 1
    for u in "$MGR_UID" "$ASK_UID" "$DENY_UID" "$FAKE_UID"; do
        probe_boot "$u" || return 1
    done
    return 0
}

boot_bad=""
if try_dir "${BOSS_BUILD_DIR:-$ROOT/build}"; then
    :
elif try_dir /tmp/boss-task6-build; then
    echo "[i] 工作区目录不能执行二进制，已改用 $BUILD"
else
    try_dir "${BOSS_BUILD_DIR:-$ROOT/build}" >/dev/null 2>&1 || try_dir /tmp/boss-task6-build >/dev/null 2>&1
    echo "SKIP 找不到一个能让 uid>10000 执行二进制的目录——能力不具备，不是代码有问题。"
    diagnose "$MGR_UID"
    exit 0
fi
echo "[ok] 探针自举通过（uid $MGR_UID/$ASK_UID/$DENY_UID/$FAKE_UID 都能 exec，构建目录 $BUILD）"

cc -O1 -std=c11 -w -DBOSS_DIR="\"$TESTDIR\"" -DBOSS_MANAGER_PKG='"com.boss.manager"' \
   -o "$BUILD/boss-task6-fixture" \
   "$BOSS_SRC"/main.c "$BOSS_SRC"/util.c "$BOSS_SRC"/policy.c "$BOSS_SRC"/pty.c \
   "$BOSS_SRC"/daemon.c "$BOSS_SRC"/client.c "$BOSS_SRC"/bossinit.c \
   "$BOSS_SRC"/applet.c "$BOSS_SRC"/resetprop.c "$BOSS_SRC"/scripts.c \
   "$BOSS_SRC"/module.c "$BOSS_SRC"/sepolicy.c "$BOSS_SRC"/sh.c \
   "$BOSS_SRC"/boot.c "$BOSS_SRC"/selinux.c "$BOSS_SRC"/sepol_backend.c \
   "$BOSS_SRC"/mntinfo.c "$BOSS_SRC"/systemless.c "$BOSS_SRC"/hide.c \
   "$BOSS_SRC"/manager.c "$BOSS_SRC"/prompt.c \
   -ldl 2>/dev/null || {
    echo "FAIL daemon 夹具编译失败（错误打出来，别静默跳过）"
    cc -O1 -std=c11 -DBOSS_DIR="\"$TESTDIR\"" -DBOSS_MANAGER_PKG='"com.boss.manager"' \
       -o "$BUILD/boss-task6-fixture" "$BOSS_SRC"/*.c -ldl 2>&1 | tail -20
    exit 1; }

rm -rf "$TESTDIR"
mkdir -p "$TESTDIR"
# 默认拒绝。65533 单独给一条 prompt 规则——它是第 5 节弹窗用例的主角。
# 其余 uid 一律走 default=deny，响应是**立即**的；
# 别让"验证会被拒"的用例走 prompt 规则，那是 60 秒等待（弹窗超时），
# 会把后面的用例全拖进"上一个请求还在等"的状态里，症状是莫名其妙的 DOWN。
printf 'default = deny\nlog = 1\nuid 65533 prompt\n' > "$TESTDIR/policy.conf"

"$BUILD/boss-task6-fixture" daemon --foreground >/dev/null 2>&1 &
DPID=$!
cleanup() { kill "$DPID" 2>/dev/null; wait "$DPID" 2>/dev/null; }
trap cleanup EXIT
for _ in $(seq 1 50); do "$BUILD/boss-task6-fixture" ping >/dev/null 2>&1 && break; sleep 0.1; done

# 伪装成 App：cmdline 必须是包名（真机上由 zygote 填）
# HOME 指到 /tmp、--noprofile --norc：非 root 身份读 /root/.bashrc 会报
# Permission denied，噪声会污染我们按行解析的输出。
run_as() {
    setpriv --reuid="$1" --regid="$1" --clear-groups \
        env HOME=/tmp bash --noprofile --norc -c "exec -a $2 $PROBE $3"
}
as_manager() { run_as "$MGR_UID"  com.boss.manager      "$1"; }
as_asker()   { run_as "$ASK_UID"  com.example.notboss   "$1"; }

echo
echo "=== 1) 没注册之前：普通 app 被拒 ==="
out="$(run_as "$DENY_UID" com.example.notboss 'run echo hello' 2>&1)"
echo "$out" | grep -q '^code=1' && check 0 "默认 deny：不认识的 uid 被拒绝" \
                                || { check 1 "默认 deny：不认识的 uid 被拒绝"; echo "$out"; }

echo
echo "=== 2) manager 抢注（首次开机窗口）==="
out="$(as_manager 'ui manager' 2>&1)"
echo "$out" | grep -q '^code=0' && check 0 "manager 首次注册成功（uid=$MGR_UID）" \
                                 || { check 1 "manager 首次注册成功"; echo "$out"; }
[ -f "$TESTDIR/manager.uid" ] && check 0 "manager.uid 已落盘" || check 1 "manager.uid 已落盘"

alive() { "$BUILD/boss-task6-fixture" ping 2>&1 | grep -q up; }
alive || { echo "daemon 没了，后续用例无意义"; exit 1; }

echo
echo "=== 3) 已注册后：抢注窗口关闭 ==="
out="$(run_as "$FAKE_UID" com.boss.manager 'ui manager' 2>&1)"
echo "$out" | grep -q '^code=1' && check 0 "另一个 uid 冒充 manager 被拒（顶不掉）" \
                                 || { check 1 "另一个 uid 冒充 manager 被拒"; echo "$out"; }

echo
echo "=== 4) manager 自动放行（鸡生蛋解开）==="
out="$(as_manager 'run echo hello' 2>&1)"
echo "$out" | grep -q '^code=0' && check 0 "manager 无需 policy 规则即可执行" \
                                 || { check 1 "manager 无需 policy 规则即可执行"; echo "$out"; }
echo "$out" | grep -q 'hello' && check 0 "manager 拿到子进程输出" \
                              || { check 1 "manager 拿到子进程输出"; echo "$out"; }

echo
echo "=== 5) 授权弹窗：真的等用户点 ==="
# 后台发起一个会进 prompt 的请求（uid 65533 的策略是 prompt）
( as_asker 'run echo popup-ok' > "$TESTDIR/asker.out" 2>&1 ) &
ASKER=$!

# 等 .req 出现（它就是"弹窗"本身）
id=""
for _ in $(seq 1 60); do
    f="$(ls "$TESTDIR/prompt/" 2>/dev/null | grep '\.req$' | head -1)"
    [ -n "$f" ] && { id="${f%.req}"; break; }
    sleep 0.1
done
[ -n "$id" ] && check 0 "待决请求已生成 id=$id" || { check 1 "待决请求已生成"; }

if [ -n "$id" ]; then
    grep -q 'uid=65533' "$TESTDIR/prompt/$id.req" 2>/dev/null \
        && check 0 "请求里记的是内核给的 uid=65533" \
        || check 1 "请求里记的是内核给的 uid"

    out="$(as_manager "ui pending" 2>&1)"
    echo "$out" | grep -q "$id" && check 0 "App 通过 UI 通道看到待决请求" \
                                || { check 1 "App 通过 UI 通道看到待决请求"; echo "$out"; }

    out="$(as_manager "ui allow $id" 2>&1)"
    echo "$out" | grep -q '^code=0' && check 0 "App 裁决 allow 被 daemon 接受" \
                                    || { check 1 "App 裁决 allow 被 daemon 接受"; echo "$out"; }
fi

wait "$ASKER" 2>/dev/null
out="$(cat "$TESTDIR/asker.out" 2>/dev/null)"
echo "$out" | grep -q 'popup-ok' && check 0 "用户放行后命令真的跑起来了" \
                                 || { check 1 "用户放行后命令真的跑起来了"; echo "$out"; }

echo
echo "=== 6) 弹窗被拒绝：不能默认放行 ==="
( as_asker 'run echo should-not-run' > "$TESTDIR/asker2.out" 2>&1 ) &
ASKER2=$!
id2=""
for _ in $(seq 1 60); do
    f="$(ls "$TESTDIR/prompt/" 2>/dev/null | grep '\.req$' | head -1)"
    [ -n "$f" ] && { id2="${f%.req}"; break; }
    sleep 0.1
done
if [ -n "$id2" ]; then
    as_manager "ui deny $id2" >/dev/null 2>&1
    wait "$ASKER2" 2>/dev/null
    out="$(cat "$TESTDIR/asker2.out" 2>/dev/null)"
    echo "$out" | grep -q '^code=1' && check 0 "拒绝后返回 DENIED（没有默认放行）" \
                                    || { check 1 "拒绝后返回 DENIED"; echo "$out"; }
    echo "$out" | grep -q 'should-not-run' \
        && { check 1 "拒绝后命令没有执行"; } || check 0 "拒绝后命令没有执行"
else
    check 1 "第二次弹窗生成了待决请求"
fi

echo
echo "=== 7) 裁决后不留痕 ==="
n="$(ls "$TESTDIR/prompt/" 2>/dev/null | wc -l)"
[ "$n" = "0" ] && check 0 "prompt 目录已清空（不留下可枚举的痕迹）" \
               || check 1 "prompt 目录残留 $n 个文件"

echo
[ "$fails" = "0" ] && echo "结果：PASS 全部通过" || echo "结果：FAIL $fails 项"
exit "$fails"
