#!/bin/bash
# BOSS · 任务5 B 面验收：特典逻辑（隐藏名单 / 挂载痕迹治理 / 属性伪装）
#
# 设计原则与 smoke_test 一致：**root 与非 root 都要能跑完**。
#   · 非 root 下 setns/umount 必然失败，脚本要明确标 SKIP，不许静默通过；
#   · 更不许因为"提权失败"就让整条链路在 CI 上红掉（任务3 坑 7 的教训）。
#
# 运行：bash tools/hide_test.sh
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
echo "== 1. 隐藏名单：增删查与幂等 =="
$BIN hide denylist add com.example.bank >/dev/null 2>&1
$BIN hide denylist add com.example.bank >/dev/null 2>&1
N=$($BIN hide denylist list 2>/dev/null | grep -c "com.example.bank")
check "重复 add 不产生重复项" "$N" "1"
$BIN hide denylist add com.example.pay >/dev/null 2>&1
N=$($BIN hide denylist list 2>/dev/null | wc -l)
check "第二项能加入" "$N" "2"
$BIN hide denylist del com.example.pay >/dev/null 2>&1
N=$($BIN hide denylist list 2>/dev/null | wc -l)
check "del 生效" "$N" "1"
[ -f "$TEST_DIR/denylist.conf" ] && ok "名单落在 BOSS_DIR 下（/system 无痕）" || bad "名单路径不对"

echo
echo "== 2. 暴露面清单 =="
OUT=$($BIN hide mounts 2>&1)
echo "$OUT" | grep -q "BOSS 引入的挂载" && ok "mounts 可跑" || bad "mounts 失败"
echo "$OUT" | grep -q "挂载表总条数" && ok "报出挂载表总条数（说明真读了 mountinfo）" || bad "没读挂载表"
$BIN hide mounts --write >/dev/null 2>&1
[ -f "$TEST_DIR/mount.log" ] && ok "挂载快照可落盘" || bad "mount.log 未生成"

echo
echo "== 3. 目标进程摘挂载 =="
if [ "$UID_NOW" -eq 0 ]; then
    OUT=$($BIN hide umount $$ --dry 2>&1; echo "RC=$?")
    echo "$OUT" | grep -q "RC=0" && ok "dry run 对自身 pid 成功" || bad "dry run 失败: $OUT"
    # 真摘：沙盒里没有 BOSS 挂载，所以这一步应当"什么都没摘"而不是报错
    OUT=$($BIN hide umount $$ 2>&1; echo "RC=$?")
    echo "$OUT" | grep -q "RC=0" && ok "无 BOSS 挂载时返回 0（不是误报失败）" || bad "真摘返回非 0: $OUT"
else
    skip "非 root：setns 需要 CAP_SYS_ADMIN"
fi

echo
echo "== 4. 进程扫描 =="
OUT=$($BIN hide scan 2>&1; echo "RC=$?")
echo "$OUT" | grep -q "RC=0" && ok "scan 不崩且返回 0" || bad "scan 失败"
[ -f "$TEST_DIR/hide.state" ] && ok "命中后写入去重状态（避免空转）" || skip "本轮没有命中任何进程"

echo
echo "== 5. 守护进程：--once 必须能退出 =="
timeout 10 $BIN hide daemon --once >/dev/null 2>&1
RC=$?
check "daemon --once 会退出（不会挂住开机）" "$RC" "0"

echo
echo "== 6. 属性伪装清单 =="
$BIN hide props --save-template >/dev/null 2>&1
[ -f "$TEST_DIR/props.conf" ] && ok "模板已生成" || bad "模板未生成"
grep -q "不承诺" "$TEST_DIR/props.conf" && ok "模板里写明了能力边界" || bad "模板缺少边界说明"
grep -q "^prop ro.debuggable 0" "$TEST_DIR/props.conf" && ok "含 ro.debuggable 条目" || bad "缺关键条目"
# 应用它（沙盒里属性区不存在，resetprop 会失败，这里只验"链路通、不崩"）
timeout 30 $BIN hide props >/dev/null 2>&1
RC=$?
[ "$RC" -le 3 ] && ok "应用链路可跑完（返回码在 0~3 语义内，实际 rc=$RC）" || bad "应用返回异常 rc=$RC"

echo
echo "== 7. 注册与帮助 =="
$BIN --list 2>&1 | grep -q "hide" && ok "applet 表已注册 hide" || bad "未注册"
RC=$($BIN hide >/dev/null 2>&1; echo $?)
check "无子命令返回 1" "$RC" "1"

echo
echo "结果：PASS=$PASS FAIL=$FAIL SKIP=$SKIP"
[ "$FAIL" -eq 0 ]
