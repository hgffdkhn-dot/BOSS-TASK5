#!/bin/bash
# BOSS · 向下兼容验收（su 与 init）
#
# 覆盖三件事：
#   A. rc 语法下限：这一份 rc 必须能被 Android 5/6/7 的老 init 解析
#   B. init 布局探测：2SI 与非 2SI 走向不同的执行链，且判定可被覆盖
#   C. su 协议与命令行：新/老二进制混着用仍能工作（真机上两份 boss 会错版本）
#
# 同样要能 root 与非 root 两种身份跑（CI runner 是非 root）。
#
# 运行：bash tools/compat_test.sh
set -u
cd "$(dirname "$0")/.."

PASS=0; FAIL=0; SKIP=0
ok()   { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad()  { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
skip() { echo "  [SKIP] $1"; SKIP=$((SKIP+1)); }
check(){ if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (期望 '$3' 实际 '$2')"; fi; }

UID_NOW=$(id -u)
if [ "$UID_NOW" -eq 0 ]; then EXPECT_UID=0; else EXPECT_UID=$UID_NOW; fi
TEST_DIR=/tmp/boss-test

echo "== 构建 =="
make test >/dev/null 2>&1 || { echo "构建失败"; exit 1; }
make initkit >/dev/null 2>&1 || { echo "initkit 构建失败"; exit 1; }
BIN=./build/boss
KIT=./build/initkit

# 只发 v1 的"老客户端"：验证新 daemon 能接住老请求。
# -DBOSS_PROTO_VER=1 能生效的前提是 boss.h 里那个宏用 #ifndef 包住——
# 包成无条件 #define 的话，这里的 -D 会被盖掉，编出来仍是 v2，测试假绿。
V1CLIENT=./build/boss-v1client
cc -O2 -std=c11 -w -DBOSS_DIR='"/tmp/boss-test"' -DBOSS_PROTO_VER=1 \
   -o "$V1CLIENT" src/*.c -ldl 2>/dev/null \
   || { echo "老客户端构建失败"; exit 1; }

stop_daemon() { pkill -x boss >/dev/null 2>&1; sleep 0.2; }
stop_daemon
rm -rf "$TEST_DIR" 2>/dev/null
if [ -e "$TEST_DIR" ]; then
    if [ "$UID_NOW" -ne 0 ] && command -v sudo >/dev/null 2>&1; then sudo rm -rf "$TEST_DIR" 2>/dev/null; fi
fi
if [ -e "$TEST_DIR" ]; then echo "ERROR: 无法清理 $TEST_DIR" >&2; exit 1; fi
mkdir -p "$TEST_DIR"
printf 'default = deny\nlog = 1\nuid %s allow\n' "$UID_NOW" > "$TEST_DIR/policy.conf"

echo
echo "== A. rc 语法下限（老 init 能不能解析）=="
RC=payload/init.boss.rc
# 只看真正的指令行：注释里会**提到**这些关键字（说明为什么不能用），
# 直接 grep 全文会把自己的说明文字当成违规，测试永远红。
sed 's/#.*$//' "$RC" > /tmp/boss-compat-rc-nocomment
RCNC=/tmp/boss-compat-rc-nocomment
if grep -q "exec_background" "$RCNC"; then
    bad "rc 用了 exec_background（Android 8.0+ 才有，老 init 会拒绝整段）"
else
    ok "rc 未使用 exec_background"
fi
if grep -qE "^\s*seclabel" "$RCNC"; then
    bad "rc 用了 seclabel（老 init 上是未知选项）"
else
    ok "rc 未使用 seclabel"
fi
# 每个 start X 都要有对应的 service X：老 init 下 start 一个不存在的服务
# 只会打一行 "service not found"，然后这个阶段就静默地什么都不做
MISSING=0
for n in $(grep -oE "^\s*start [a-zA-Z0-9_]+" "$RCNC" | awk '{print $2}' | sort -u); do
    grep -qE "^service $n " "$RCNC" || { echo "      start 了不存在的服务: $n"; MISSING=1; }
done
check "所有 start 都有对应 service 定义" "$MISSING" "0"
# 服务名 ≤16（老 init 对服务名长度有限制）
LONG=0
for n in $(grep -oE "^service [a-zA-Z0-9_]+" "$RCNC" | awk '{print $2}'); do
    [ ${#n} -gt 16 ] && { echo "      服务名过长: $n"; LONG=1; }
done
check "服务名均 ≤16 字符" "$LONG" "0"

echo
echo "== B. init 布局探测 =="
OUT=$(BOSS_LAYOUT=2si $KIT probe 2>/dev/null)
echo "$OUT" | grep -q "^LAYOUT:2si$" && ok "2si 判定" || bad "2si 判定失败: $OUT"
echo "$OUT" | grep -q "^HIJACK_NEEDED:1$" && ok "2si 下需要劫持" || bad "HIJACK_NEEDED 不对"

OUT=$(BOSS_LAYOUT=legacy_root $KIT probe 2>/dev/null)
echo "$OUT" | grep -q "^LAYOUT:legacy_root$" && ok "legacy_root 判定" || bad "legacy 判定失败: $OUT"
echo "$OUT" | grep -q "^HIJACK_NEEDED:0$" && ok "老布局不需要劫持" || bad "老布局仍要劫持"
echo "$OUT" | grep -q "^RAMDISK_RC_EFFECTIVE:1$" && ok "老布局上 ramdisk rc 有效" || bad "RAMDISK_RC_EFFECTIVE 不对"

echo
echo "== B2. 老布局上 hijack 必须跳过（否则会遮住 /sdcard）=="
OUT=$(BOSS_LAYOUT=legacy_root $KIT hijack-prep 2>&1); RC2=$?
check "跳过时仍返回 0（不拖住 init）" "$RC2" "0"
echo "$OUT" | grep -q "跳过" && ok "说明了跳过原因" || bad "没说为什么跳过: $OUT"

echo
echo "== B3. 2si 下 dry run 仍要给出完整计划（不被跳过逻辑吃掉）=="
OUT=$(BOSS_LAYOUT=2si $KIT hijack-prep --dry 2>&1)
echo "$OUT" | grep -q "would bind" && ok "2si dry run 给出挂载计划" || bad "dry run 被跳过逻辑吃掉: $OUT"

echo
echo "== C. su 命令行向下兼容（老应用传的参数）=="
$BIN daemon >/dev/null 2>&1
for i in $(seq 1 24); do $BIN ping >/dev/null 2>&1 && break; sleep 0.25; done

check "su -V 打印版本" "$($BIN su -V >/dev/null 2>&1; echo $?)" "0"
check "su -v 打印版本（老脚本写法）" "$($BIN su -v >/dev/null 2>&1; echo $?)" "0"
check "su -M（mount-master）不报错" "$($BIN su -M -c 'true' >/dev/null 2>&1; echo $?)" "0"
check "su --mount-master 同上" "$($BIN su --mount-master -c 'true' >/dev/null 2>&1; echo $?)" "0"
check "su -Z 指定域不报错" "$($BIN su -Z u:r:init:s0 -c 'true' >/dev/null 2>&1; echo $?)" "0"

# -c 之后多个 token 要拼成一条命令（老应用常见 `su -c ls -l`）
OUT=$($BIN su -c echo a b 2>/dev/null | tr -d '\r' | tr '\n' ' ')
check "-c 后多参数被拼成一条命令" "$OUT" "a b "
# 位置参数当目标用户（su root -c ...）
check "su root -c 可执行" "$($BIN su root -c 'true' >/dev/null 2>&1; echo $?)" "0"
check "su 0 -c 可执行" "$($BIN su 0 -c 'true' >/dev/null 2>&1; echo $?)" "0"
# 未知选项要被接受而不是报错（老应用传得比我们实现的多）
check "未知选项被忽略而非报错" "$($BIN su --some-unknown-flag -c 'true' >/dev/null 2>&1; echo $?)" "0"

echo
echo "== C2. 老客户端 → 新 daemon（v1 请求必须被接住）=="
check "v1 客户端能连上 v2 daemon" "$($V1CLIENT su -c 'true' >/dev/null 2>&1; echo $?)" "0"
OUT=$($V1CLIENT su -c 'id -u' 2>/dev/null)
check "v1 客户端执行结果正确" "$OUT" "$EXPECT_UID"
if grep -q "proto=v1" "$TEST_DIR/boss.log" 2>/dev/null; then
    ok "审计日志记下了 proto=v1（混版本部署时的唯一线索）"
else
    bad "日志里没有 proto=v1"
fi

echo
echo "== C3. 新客户端 → 只认 v1 的老 daemon（必须自动降级）=="
stop_daemon
rm -f "$TEST_DIR/boss.log" 2>/dev/null
printf 'default = deny\nlog = 1\nuid %s allow\n' "$UID_NOW" > "$TEST_DIR/policy.conf"
BOSS_PROTO_MAX=1 $BIN daemon >/dev/null 2>&1
for i in $(seq 1 24); do $BIN ping >/dev/null 2>&1 && break; sleep 0.25; done
# 注：`boss ping` 本身也会降级，所以这里能 ping 通就是降级生效的证据之一
check "探活在对端只认 v1 时仍成功" "$($BIN ping >/dev/null 2>&1; echo $?)" "0"
OUT=$($BIN su -c 'id -u' 2>/dev/null)
check "新客户端降级后仍能执行" "$OUT" "$EXPECT_UID"
if grep -q "proto=v1" "$TEST_DIR/boss.log" 2>/dev/null; then
    ok "降级后按 v1 送达（日志可见）"
else
    bad "日志里没有 proto=v1，降级可能没发生"
fi
if grep -q "拒绝请求：协议版本" "$TEST_DIR/boss.log" 2>/dev/null; then
    ok "老 daemon 确实拒过一次 v2（说明降级路径真被走到了）"
else
    skip "日志里没有拒绝记录（v2 请求可能一次就被接受）"
fi

stop_daemon
rm -f "$V1CLIENT"
echo
echo "结果：PASS=$PASS FAIL=$FAIL SKIP=$SKIP"
[ "$FAIL" -eq 0 ]
