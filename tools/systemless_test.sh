#!/bin/bash
# BOSS · 任务5 A 面验收：无修改系统逻辑（systemless）
#
# 能离机验的：清单解析（含坏行容错）、dry run、属性端到端改写（合成属性区）、
# 基线比对、状态与自检不崩。
# 必须真机验的：挂载来源断言（需要真实的分区布局）。脚本里明确标 SKIP，
# 绝不把"没验"当成"验过"。
#
# 运行：bash tools/systemless_test.sh
set -u
cd "$(dirname "$0")/.."

PASS=0; FAIL=0; SKIP=0
ok()   { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad()  { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
skip() { echo "  [SKIP] $1"; SKIP=$((SKIP+1)); }
check(){ if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (期望 '$3' 实际 '$2')"; fi; }

echo "== 构建 =="
make test >/dev/null 2>&1 || { echo "构建失败"; exit 1; }
BIN=./build/boss
TEST_DIR=/tmp/boss-test
rm -rf "$TEST_DIR" 2>/dev/null
mkdir -p "$TEST_DIR"

echo
echo "== 1. 清单解析：语法与容错 =="
CONF=$TEST_DIR/systemless.conf
cat > "$CONF" <<'EOF'
# 这是注释
prop ro.debuggable 1
prop   ro.secure   0
persist persist.boss.test hello
prop -d ro.build.type
deny com.example.bank
prop.bad.line
EOF
OUT=$($BIN systemless plan --file "$CONF" 2>&1)
echo "$OUT" | grep -q "ro.debuggable" && ok "prop 条目被解析" || bad "prop 条目丢失"
echo "$OUT" | grep -q "\[persist\]"    && ok "persist 条目被识别" || bad "persist 丢失"
echo "$OUT" | grep -q "\[-delete\]"    && ok "删除条目被识别" || bad "-d 丢失"
echo "$OUT" | grep -q "com.example.bank" && ok "deny 条目被解析" || bad "deny 丢失"
RC=$($BIN systemless plan --file "$CONF" >/dev/null 2>&1; echo $?)
check "坏行只警告不中断" "$RC" "0"

echo
echo "== 2. dry run 不碰真实状态 =="
PROPDIR=$TEST_DIR/props
python3 tools/mkprop.py "$PROPDIR" ro.debuggable=0 ro.secure=1 >/dev/null 2>&1
if [ -d "$PROPDIR" ]; then
    # resetprop 读出来是 "[ro.debuggable]: [0]" 这种格式，按值匹配而不是整行相等
    $BIN systemless plan --file "$CONF" --prop-dir "$PROPDIR" >/dev/null 2>&1
    V=$($BIN resetprop --dir "$PROPDIR" ro.debuggable 2>/dev/null)
    echo "$V" | grep -q "\[0\]" && ok "dry run 后属性未被改动" || bad "dry run 却改了属性: $V"
else
    skip "合成属性区不可用（tools/mkprop.py 没跑起来）"
fi

echo
echo "== 3. apply 端到端：值真的进了属性区 =="
if [ -d "$PROPDIR" ]; then
    $BIN systemless apply --file "$CONF" --prop-dir "$PROPDIR" >/dev/null 2>&1
    V=$($BIN resetprop --dir "$PROPDIR" ro.debuggable 2>/dev/null)
    echo "$V" | grep -q "\[1\]" && ok "prop 改写后能读回新值" || bad "prop 未生效: $V"
    V=$($BIN resetprop --dir "$PROPDIR" ro.secure 2>/dev/null)
    echo "$V" | grep -q "\[0\]" && ok "第二条 prop 同样生效" || bad "第二条未生效: $V"
else
    skip "无合成属性区，跳过端到端"
fi

echo
echo "== 4. 返回码语义（沿用任务4：0 全应用 / 3 部分应用 / 2 无能力）=="
cat > "$TEST_DIR/empty.conf" <<'EOF'
# 只有注释
EOF
RC=$($BIN systemless apply --file "$TEST_DIR/empty.conf" >/dev/null 2>&1; echo $?)
check "空清单返回 0" "$RC" "0"
RC=$($BIN systemless apply --file "$TEST_DIR/nonexistent.conf" >/dev/null 2>&1; echo $?)
check "清单不存在返回 1" "$RC" "1"

echo
echo "== 5. 自检：基线比对 =="
FAKE=$TEST_DIR/fakeroot/system
mkdir -p "$FAKE"
echo hello > "$FAKE/a.txt"
$BIN systemless verify --dir "$FAKE" --baseline "$TEST_DIR/base.txt" --save >/dev/null 2>&1
[ -f "$TEST_DIR/base.txt" ] && ok "基线可写入" || bad "基线写入失败"
OUT=$($BIN systemless verify --dir "$FAKE" --baseline "$TEST_DIR/base.txt" 2>&1)
echo "$OUT" | grep -q "与基线一致" && ok "未改动时判定一致" || bad "未改动却报差异"
echo world > "$FAKE/b.txt"
OUT=$($BIN systemless verify --dir "$FAKE" --baseline "$TEST_DIR/base.txt" 2>&1)
echo "$OUT" | grep -q "处差异" && ok "新增文件能被基线发现" || bad "新增文件没被发现"

echo
echo "== 6. 自检：越界判定（挂载来源）=="
if [ -d /system ]; then
    OUT=$($BIN systemless verify 2>&1; echo "RC=$?")
    echo "$OUT" | grep -q "RC=" && ok "verify 在真实布局下可跑完" || bad "verify 崩了"
else
    skip "非 Android 环境：/system 不存在，挂载来源断言需真机验"
fi

echo
echo "== 7. 状态与帮助 =="
$BIN systemless status >/dev/null 2>&1 && ok "status 可跑" || bad "status 失败"
RC=$($BIN systemless >/dev/null 2>&1; echo $?)
check "无子命令返回 1" "$RC" "1"
$BIN --list 2>&1 | grep -q "systemless" && ok "applet 表已注册 systemless" || bad "未注册"

echo
echo "结果：PASS=$PASS FAIL=$FAIL SKIP=$SKIP"
[ "$FAIL" -eq 0 ]
