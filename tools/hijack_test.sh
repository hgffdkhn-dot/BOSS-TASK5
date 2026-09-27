#!/bin/bash
# BOSS · 任务5 补完的 v0.2 前置：SwitchRoot 劫持布置（hijack-prep）
#
# ⚠️ 本脚本的第一版在 CI 上全红过，原因是踩了接力须知坑 7：
#    `init` 这个 applet 标了 needs_root=1，非 root 下 applet 分发在调用 fn
#    **之前**就返回 1。而 GitHub runner 是非 root——本地 root 全绿、CI 全红。
#
# 修法不是去改产品的 root 语义（needs_root 对真机 rc 脚本是正确的：
# 非 root 时明确报错，不静默失败），而是让测试从 applet 层绕过去：
#    tests/initkit.c 直接调 boss_init_main()，与 tests/sepolkit.c 同类做法。
#   init 里真正需要特权的只有 mount，参数转发 / 变砖保护 / dry run 都不需要。
#
# 仍然只能真机验的：切根后的落点。沙盒没有内核、没有 SwitchRoot 可观察，
# 本地最多验到"命令拼对了"。这部分明确标 SKIP，绝不假装验过。
#
# 运行：bash tools/hijack_test.sh
set -u
cd "$(dirname "$0")/.."

PASS=0; FAIL=0; SKIP=0
ok()   { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad()  { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
skip() { echo "  [SKIP] $1"; SKIP=$((SKIP+1)); }
check(){ if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (期望 '$3' 实际 '$2')"; fi; }

UID_NOW=$(id -u)

echo "== 构建 =="
make test >/dev/null 2>&1 || { echo "构建失败"; exit 1; }
make initkit >/dev/null 2>&1 || { echo "initkit 构建失败"; exit 1; }
BIN=./build/boss
KIT=./build/initkit
TEST_DIR=/tmp/boss-test
mkdir -p "$TEST_DIR" 2>/dev/null

echo
echo "== 1. needs_root 契约：非 root 下必须明确报错，不能静默失败 =="
if [ "$UID_NOW" -eq 0 ]; then
    skip "当前是 root，这一条要在非 root 下才验得到（CI 会验）"
else
    OUT=$($BIN init hijack-prep --dry 2>&1); RC=$?
    check "applet 层拒绝并返回非 0" "$RC" "1"
    echo "$OUT" | grep -q "需要 root" && ok "报错说清了原因" || bad "静默失败: $OUT"
fi

echo
echo "== 2. dry run：说清打算做什么 =="
OUT=$($KIT hijack-prep --dry 2>&1); RC=$?
check "dry run 返回 0" "$RC" "0"
echo "$OUT" | grep -q "would bind" && ok "说清了打算挂什么" || bad "输出不完整"
echo "$OUT" | grep -q "/init.real" && ok "包含真实 init 备份这一步" || bad "缺 /init.real 备份说明"

echo
echo "== 3. dry run 不留痕迹 =="
# 比"有没有新增"而不是"路径存不存在"：第 5 项的真跑会留下 /sdcard
# 与 /storage/self，那是真跑的产物，不能算到 dry 头上。
B_LINK=0; B_REAL=0; B_SDCARD=0; B_SELF=0
[ -L /storage/self/primary ] && B_LINK=1
[ -e /init.real ] && B_REAL=1
[ -e /sdcard ] && B_SDCARD=1
[ -e /storage/self ] && B_SELF=1
$KIT hijack-prep --dry >/dev/null 2>&1
N_LINK=0; N_REAL=0; N_SDCARD=0; N_SELF=0
[ -L /storage/self/primary ] && N_LINK=1
[ -e /init.real ] && N_REAL=1
[ -e /sdcard ] && N_SDCARD=1
[ -e /storage/self ] && N_SELF=1
if [ "$B_LINK" != "$N_LINK" ] || [ "$B_REAL" != "$N_REAL" ] ||
   [ "$B_SDCARD" != "$N_SDCARD" ] || [ "$B_SELF" != "$N_SELF" ]; then
    bad "dry run 产生了副作用"
else
    ok "dry run 未产生任何新增副作用"
fi

echo
echo "== 4. 布置失败不能中断 init（这条决定开不开得了机）=="
if [ "$UID_NOW" -eq 0 ]; then
    # 真跑会真的 bind mount /sdcard。在开发者本机上做这件事既危险又没意义，
    # 而 root 环境下 mount 会成功，也验不到"失败"这条分支。
    skip "root 环境：真跑会真的挂载 /sdcard，只在非特权环境（CI）验"
else
    timeout 20 $KIT hijack-prep >/dev/null 2>&1; RC=$?
    check "mount 失败仍返回 0（init 不会被拖住）" "$RC" "0"
fi

echo
echo "== 5. 变砖保护：找不到真实 init 时绝不 exec 自己 =="
BOSS_INIT_REAL=/nonexistent/real-init timeout 20 $KIT stage2 second_stage >/dev/null 2>&1
check "返回 127（没有 exec 自己造成死循环）" "$?" "127"

echo
echo "== 6. 阶段参数不能丢（丢了就开机循环）=="
FAKE=$TEST_DIR/fakeinit.sh
cat > "$FAKE" <<'EOF'
#!/bin/bash
echo "$@" > /tmp/boss-test/args.txt
EOF
chmod +x "$FAKE"
rm -f "$TEST_DIR/args.txt"

BOSS_INIT_REAL="$FAKE" timeout 20 $KIT stage2 second_stage >/dev/null 2>&1
if [ -f "$TEST_DIR/args.txt" ]; then
    grep -q "second_stage" "$TEST_DIR/args.txt" \
        && ok "second_stage 原样转发给真实 init" || bad "阶段参数丢失"
else
    bad "假 init 未被调用（execv 这条链路断了）"
fi

rm -f "$TEST_DIR/args.txt"
BOSS_INIT_REAL="$FAKE" timeout 20 $KIT selinux_setup >/dev/null 2>&1
if [ -f "$TEST_DIR/args.txt" ]; then
    grep -qE "selinux_setup|second_stage" "$TEST_DIR/args.txt" \
        && ok "selinux_setup 被接住，且传的是合法阶段参数" || bad "参数异常"
else
    bad "selinux_setup 分支没走到 execv"
fi

echo
echo "== 7. 自救开关 =="
if [ -r /proc/cmdline ]; then
    ok "cmdline 可读（真机上 boss_selinux=0 可现场退回原厂路径）"
else
    skip "沙盒无 /proc/cmdline"
fi

# 测试过程中 start_daemon() 可能拉起过 bossd，留着会占住抽象套接字，
# 影响后续套件（表现为 Address already in use）
pkill -x boss >/dev/null 2>&1
pkill -x initkit >/dev/null 2>&1

echo
echo "结果：PASS=$PASS FAIL=$FAIL SKIP=$SKIP"
echo
echo "真机判定（沙盒做不到）：冷启动后 ls -l /proc/1/exe 指向 boss = 劫持成功，"
echo "指向原厂 init = 没成。这比翻日志快——那个阶段日志可能一个字节都没有。"
[ "$FAIL" -eq 0 ]
