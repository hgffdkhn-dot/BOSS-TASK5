#!/usr/bin/env bash
# BOSS · 任务6：IPC 链路自检（协议布局 + 端到端）
#
# 两件事分开跑，因为失败的含义完全不同：
#   ipc_layout_test  编译期断言：App 侧的协议镜像与上游 src/boss.h 错位
#   ipc_host_test    运行期链路：握手/输出/退出码真实跑一遍 bossd
#
# ⚠️ 改测试请以**非 root** 那一份为准（接力须知约束②，这条真红过一次）：
#     本脚本两种身份各跑一遍。CI 的 runner 是非 root。
#
# 用法：
#   BOSS_SRC=/path/to/BOSS-TASK5/src bash tools/run_ipc_test.sh
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
BUILD="$ROOT/build"
TESTDIR="${BOSS_TEST_DIR:-/tmp/boss-ipc-test}"

mkdir -p "$BUILD"

# ---- 预检：任务6 的 daemon 侧补丁合进去了吗？ ----
# 这一条必须先查。不查的话，症状是 ipc_layout_test.c 里那条 _Static_assert
# 报 "proto version mismatch"，而真实原因只是"补丁没合"——
# 一个指向协议错位的错误，会让人去改明明没错的地方。
if [ ! -f "$BOSS_SRC/manager.c" ] || [ ! -f "$BOSS_SRC/prompt.c" ]; then
    echo "SKIP $BOSS_SRC 里没有 manager.c / prompt.c —— 任务6 的 daemon 侧补丁没合进去。"
    echo "     先：git apply patches/0001-task6-manager-and-prompt.patch"
    exit 0
fi
if ! grep -qE 'BOSS_PROTO_VER +2u' "$BOSS_SRC/boss.h"; then
    echo "SKIP $BOSS_SRC/boss.h 还是 proto v1 —— 补丁没合（App 侧已经是 v2）。"
    echo "     先：git apply patches/0001-task6-manager-and-prompt.patch"
    exit 0
fi
echo "[ok] 补丁已合入：$BOSS_SRC（proto v2）"

echo "=== 0) 编译 daemon 夹具（上游源码 + 测试专用 BOSS_DIR）==="
# 为什么要自己编译一份、而不是直接跑上游的 build/boss：
#   上游 `make test` 的产物把 BOSS_DIR 编成了 /tmp/boss-test，
#   它会去读那边的 policy.conf，测试写进自己目录里的策略根本不生效
#   ——症状和接力须知坑 6（跑着另一个 BOSS_DIR 的二进制而不自知）一模一样。
#   另外产物名必须独立（坑 4.6）：build/boss 是上游的名字，混用会互相覆盖。
if [ ! -f "$BOSS_SRC/daemon.c" ]; then
    echo "SKIP 找不到上游 src（用 BOSS_SRC 指定），跳过运行期链路测试"
    exit 0
fi
cc -O1 -std=c11 -w -DBOSS_DIR="\"$TESTDIR\"" \
   -o "$BUILD/boss-ipc-fixture" \
   "$BOSS_SRC"/main.c "$BOSS_SRC"/util.c "$BOSS_SRC"/policy.c "$BOSS_SRC"/pty.c \
   "$BOSS_SRC"/daemon.c "$BOSS_SRC"/client.c "$BOSS_SRC"/bossinit.c \
   "$BOSS_SRC"/applet.c "$BOSS_SRC"/resetprop.c "$BOSS_SRC"/scripts.c \
   "$BOSS_SRC"/module.c "$BOSS_SRC"/sepolicy.c "$BOSS_SRC"/sh.c \
   "$BOSS_SRC"/boot.c "$BOSS_SRC"/selinux.c "$BOSS_SRC"/sepol_backend.c \
   "$BOSS_SRC"/mntinfo.c "$BOSS_SRC"/systemless.c "$BOSS_SRC"/hide.c \
   "$BOSS_SRC"/manager.c "$BOSS_SRC"/prompt.c \
   -ldl 2>/dev/null || {
    echo "FAIL daemon 夹具编译失败（错误打出来，别静默跳过）"
    cc -O1 -std=c11 -DBOSS_DIR="\"$TESTDIR\"" \
       -o "$BUILD/boss-ipc-fixture" "$BOSS_SRC"/*.c -ldl 2>&1 | tail -20
    exit 1
}
BOSS_BIN="$BUILD/boss-ipc-fixture"

echo "=== 1) 协议布局断言 ==="
cc -std=c11 -Wall -Wextra -Wno-unused-parameter \
   -I"$BOSS_SRC" -I"$ROOT/app/src/main/cpp" \
   -o "$BUILD/ipc_layout_test" "$HERE/ipc_layout_test.c" || {
    echo "FAIL 协议布局断言编译失败（_Static_assert 不过就是这里红）"
    exit 1
}
"$BUILD/ipc_layout_test" || exit 1

# 产物必须能被非 root 身份 exec：容器化 runner 的工作区可能挂了 noexec，
# 那种情况下 ipc_host_test 在第 3 步会直接 Permission denied。
# 这里用 root 自己先试一次（noexec 不看 uid，root 一样跑不了），
# 不行就换 /tmp 重编——总比报一堆指向错误方向的 FAIL 强。
if ! "$BUILD/ipc_host_test" >/dev/null 2>&1 && [ "$BUILD" != "/tmp/boss-ipc-build" ]; then
    echo "[i] $BUILD 里的二进制跑不起来，换到 /tmp 重编"
    BUILD=/tmp/boss-ipc-build
    mkdir -p "$BUILD"
    ( cd "$BUILD" && cc -O1 -std=c11 -w -DBOSS_DIR="\"$TESTDIR\"" \
        -o "$BUILD/boss-ipc-fixture" "$BOSS_SRC"/*.c -ldl 2>/dev/null
      cc -O1 -std=c11 -w -I"$ROOT/app/src/main/cpp" -I"$BOSS_SRC" \
        -o "$BUILD/ipc_host_test" "$HERE/ipc_host_test.c" \
        "$ROOT/app/src/main/cpp/boss_ipc.c" )
fi

echo
echo "=== 2) 端到端链路 ==="
cc -std=c11 -Wall -Wextra -Wno-unused-parameter \
   -I"$ROOT/app/src/main/cpp" \
   -o "$BUILD/ipc_host_test" "$HERE/ipc_host_test.c" \
   "$ROOT/app/src/main/cpp/boss_ipc.c" || {
    echo "FAIL boss_ipc.c 编译失败"
    exit 1
}

# 测试专用的运行时目录：绝不碰真机上的 /data/adb/boss
rm -rf "$TESTDIR"
mkdir -p "$TESTDIR"
# 默认策略是 deny，App 那套"manager 自动放行"在真机上才生效，
# 这里直接放行，验的是 IPC 链路本身。
printf 'default = allow\nlog = 1\n' > "$TESTDIR/policy.conf"

"$BOSS_BIN" daemon --foreground >/dev/null 2>&1 &
DPID=$!
cleanup() { kill "$DPID" 2>/dev/null; wait "$DPID" 2>/dev/null; }
trap cleanup EXIT

# 等 socket 起来（起不来时不要傻等）
for _ in $(seq 1 50); do
    "$BOSS_BIN" ping >/dev/null 2>&1 && break
    sleep 0.1
done

"$BUILD/ipc_host_test"
RC=$?

echo
echo "=== 3) 非 root 身份再跑一遍（以这一份为准）==="
if [ "$(id -u)" != "0" ]; then
    # 本来就非 root：第 2 步跑的那一份**就是**非 root 视角，已为准，不用再降。
    echo "[i] 当前 uid=$(id -u) 已是非 root —— 第 2 步那份即以非 root 身份跑的，无需重复。"
elif command -v setpriv >/dev/null 2>&1; then
    setpriv --reuid=65534 --regid=65534 --clear-groups \
        "$BUILD/ipc_host_test" 2>&1 | tail -3
    echo "（非 root 下 daemon 会把目标身份降级为自身，退出码/输出仍应正确）"
else
    echo "SKIP 有 root 但没有 setpriv，降不下去（本机装 util-linux 即可）"
fi

exit "$RC"
