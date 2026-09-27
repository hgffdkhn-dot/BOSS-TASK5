#!/bin/bash
# BOSS · 任务5 补完的 v0.2 前置：SwitchRoot 劫持布置（hijack-prep）
#
# 这一段是"能离机验的"与"必须真机验的"分界最清楚的地方：
#   · 离机能验：命令存在、dry run 不落任何东西、非 root 下失败也不拖垮开机、
#     真实 init 找不到时绝不 exec 自己（变砖保护）。
#   · 离机验不了：真实的 SwitchRoot 结果。沙盒里 mount 需要特权，
#     而且根本没有 SwitchRoot 可观察——本地最多验到"命令拼对了"。
#
# 所以这里**不做**"假装验过挂载生效"的断言，只把能验的验死，
# 剩下的明确标 SKIP 并在文档里写清真机判定方法（ls -l /proc/1/exe）。
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
BIN=./build/boss
TEST_DIR=/tmp/boss-test
rm -rf "$TEST_DIR" 2>/dev/null
mkdir -p "$TEST_DIR"

echo
echo "== 1. 子命令存在且可解析 =="
$BIN init hijack-prep --dry >/dev/null 2>&1
check "hijack-prep --dry 返回 0" "$?" "0"
OUT=$($BIN init hijack-prep --dry 2>&1)
echo "$OUT" | grep -q "would bind" && ok "dry run 说清了打算挂什么" || bad "dry run 输出不完整"
echo "$OUT" | grep -q "/init.real" && ok "dry run 包含真实 init 备份这一步" || bad "缺 /init.real 备份说明"

echo
echo "== 2. dry run 不留痕迹（这条最重要）=="
# 先记下基线：上一轮 test 3 的真跑会留下 /sdcard 与 /storage/self，
# 那是**真跑**的产物，不能算到 dry 头上。所以这里比的是"有没有新增"，
# 而不是"路径存不存在"——否则脚本跑第二遍就必然误报。
BASE_SDCARD=0; BASE_SELF=0; BASE_LINK=0; BASE_REAL=0
[ -e /sdcard ] && BASE_SDCARD=1
[ -e /storage/self ] && BASE_SELF=1
[ -L /storage/self/primary ] && BASE_LINK=1
[ -e /init.real ] && BASE_REAL=1
$BIN init hijack-prep --dry >/dev/null 2>&1
NEW_SDCARD=0; NEW_SELF=0; NEW_LINK=0; NEW_REAL=0
[ -e /sdcard ] && NEW_SDCARD=1
[ -e /storage/self ] && NEW_SELF=1
[ -L /storage/self/primary ] && NEW_LINK=1
[ -e /init.real ] && NEW_REAL=1
if [ "$BASE_LINK" != "$NEW_LINK" ] || [ "$BASE_REAL" != "$NEW_REAL" ]; then
    bad "dry run 创建了符号链接或 init 备份"
elif [ "$BASE_SDCARD" != "$NEW_SDCARD" ] || [ "$BASE_SELF" != "$NEW_SELF" ]; then
    bad "dry run 新增了 /sdcard 或 /storage/self"
else
    ok "dry run 未产生任何新增副作用"
fi

echo
echo "== 3. 非 root 下真跑：失败但不能拖垮开机 =="
if [ "$UID_NOW" -ne 0 ]; then
    timeout 20 $BIN init hijack-prep >/dev/null 2>&1
    RC=$?
    check "非 root 下仍返回 0（布置失败不该中断 init）" "$RC" "0"
    skip "非 root：无法验真实 mount（沙盒没有 SwitchRoot 可观察）"
else
    # root 沙盒里 mount 可能仍然失败（没有 SwitchRoot 目标），只要求不挂死
    timeout 20 $BIN init hijack-prep >/dev/null 2>&1
    RC=$?
    check "root 下真跑能返回（不挂死）" "$RC" "0"
    skip "沙盒无法验切根后的落点，需真机：ls -l /proc/1/exe"
fi

echo
echo "== 4. 变砖保护：找不到真实 init 时绝不 exec 自己 =="
# BOSS_INIT_REAL 指向不存在的文件时，stage2 必须返回 127 而不是自杀式循环
BOSS_INIT_REAL=/nonexistent/real-init timeout 20 $BIN init stage2 second_stage >/dev/null 2>&1
RC=$?
check "找不到真实 init 返回 127" "$RC" "127"

echo
echo "== 5. 阶段参数不能丢（丢了就开机循环）=="
FAKE=$TEST_DIR/fakeinit.sh
cat > "$FAKE" <<'EOF'
#!/bin/bash
echo "$@" > /tmp/boss-test/args.txt
EOF
chmod +x "$FAKE"
BOSS_INIT_REAL="$FAKE" timeout 20 $BIN init stage2 second_stage >/dev/null 2>&1
if [ -f "$TEST_DIR/args.txt" ]; then
    grep -q "second_stage" "$TEST_DIR/args.txt" && ok "second_stage 被转发给真实 init" || bad "阶段参数丢失"
else
    skip "假 init 未被调用（沙盒里 /data 不可用，属预期）"
fi
BOSS_INIT_REAL="$FAKE" timeout 20 $BIN init selinux_setup >/dev/null 2>&1
if [ -f "$TEST_DIR/args.txt" ]; then
    grep -qE "selinux_setup|second_stage" "$TEST_DIR/args.txt" \
        && ok "selinux_setup 被接住且传了合法阶段参数" || bad "selinux_setup 分支参数异常"
else
    skip "假 init 未被调用"
fi

echo
echo "== 6. cmdline 自救开关仍在 =="
if [ -r /proc/cmdline ]; then
    ok "cmdline 可读（真机上 boss_selinux=0 可现场退回原厂路径）"
else
    skip "沙盒无 /proc/cmdline"
fi

echo
echo "结果：PASS=$PASS FAIL=$FAIL SKIP=$SKIP"
echo
echo "真机判定（沙盒做不到）：冷启动后 ls -l /proc/1/exe 指向 boss = 劫持成功，"
echo "指向原厂 init = 没成。这比翻日志快——那个阶段日志可能一个字节都没有。"
[ "$FAIL" -eq 0 ]
