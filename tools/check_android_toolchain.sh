#!/usr/bin/env bash
# BOSS · Android 工具链自检（本地跑，不需要 SDK 也能跑大半）
#
# 为什么要有它：
#   AAR 元数据校验失败时会甩出二三十条
#   "requires Android Gradle plugin 9.1.0 / requires compileSdk 37"，
#   而真实原因往往只有一条：版本链上某一节没跟上。
#   那堆报错里没有任何一条会告诉你"哪一节错了、该改成多少"。
#
#   所以先把版本**原样打出来**，对着下限逐条校验，
#   在编之前就把问题定位到具体那一行。
#
#   它不需要 Android SDK：文件层面的检查全部能做。
#   有 ANDROID_HOME 时额外查平台与 build-tools 装没装。
#
# 用法：bash tools/check_android_toolchain.sh
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

fails=0; warns=0
ok()   { echo "  ok    $1"; }
bad()  { echo "  FAIL  $1"; fails=$((fails+1)); }
warn() { echo "  warn  $1"; warns=$((warns+1)); }

ver_ge() { [ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -1)" = "$2" ]; }
get() { grep -m1 -E "^$1 *=" "$ROOT/gradle/libs.versions.toml" 2>/dev/null \
        | grep -oE '[0-9]+\.[0-9]+(\.[0-9]+)?(-[A-Za-z0-9]+)?'; }

echo "BOSS · Android 工具链自检"
echo

echo "== 1) 版本目录（gradle/libs.versions.toml）=="
if [ ! -f "$ROOT/gradle/libs.versions.toml" ]; then
    bad "gradle/libs.versions.toml 不存在 —— gradle/ 整个目录没提交进来？"
    echo; echo "结果：FAIL=$fails（这份检查没法继续）"; exit 1
fi
agp="$(get agp)"; kotlin="$(get kotlin)"; bom="$(get composeBom)"; m3="$(get material3)"
echo "  agp        = ${agp:-未读到}"
echo "  kotlin     = ${kotlin:-未读到}"
echo "  composeBom = ${bom:-未读到}"
echo "  material3  = ${m3:-未读到}"

echo
echo "== 2) 版本链（四节，动一个必须一起动）=="
#   compose 1.12.0（BOM 2026.08.00 锁的）→ AAR 要求 compileSdk ≥ 37 + AGP ≥ 9.1.0
#   compileSdk 37                → AGP ≥ 9.1.1（9.0.x 最高只收 36）
#   AGP 9.1.1                    → Gradle ≥ 9.3.1（官方兼容表的硬下限）
if [ -z "$agp" ]; then bad "agp 没读到"
elif ver_ge "$agp" "9.1.0"; then ok "AGP $agp ≥ 9.1.0（Compose 1.12.0 的硬要求）"
else bad "AGP $agp < 9.1.0 —— Compose 1.12.0 的 AAR 校验会硬失败，压不掉" ; fi

if [ -z "$agp" ]; then :
elif ver_ge "$agp" "9.1.1"; then ok "AGP $agp ≥ 9.1.1（支持 API 37）"
else warn "AGP $agp < 9.1.1 —— 官方说 9.0.x 最高只接受 compileSdk 36"; fi

if [ -n "$kotlin" ] && ver_ge "$kotlin" "2.3.0" && ver_ge "$agp" "9.0.28"; then
    ok "Kotlin $kotlin + AGP $agp（Kotlin 2.3 要求 AGP ≥ 9.0.28，满足）"
elif [ -n "$kotlin" ]; then
    warn "Kotlin $kotlin 与 AGP $agp 的配对没查过（2.3 需要 AGP ≥ 9.0.28）"
fi

echo
echo "== 3) app/build.gradle.kts =="
cs="$(grep -m1 -E '^ *compileSdk *=' "$ROOT/app/build.gradle.kts" 2>/dev/null | grep -oE '[0-9]+')"
ts="$(grep -m1 -E '^ *targetSdk *=' "$ROOT/app/build.gradle.kts" 2>/dev/null | grep -oE '[0-9]+')"
echo "  compileSdk = ${cs:-未读到}"
echo "  targetSdk  = ${ts:-未读到}"
[ "$cs" = "37" ] && ok "compileSdk 37（与依赖要求一致）" \
                 || bad "compileSdk=${cs:-?}，期望 37"
# targetSdk 刻意留在 36：Android 16 的 edge-to-edge/预测式返回在 36 上已强制，
# 推到 37 会引入一批没验过的运行时行为变更。
[ "$ts" = "36" ] && ok "targetSdk 36（刻意不跟 compileSdk 走）" \
                 || warn "targetSdk=$ts（期望 36；37 会引入未验的运行时行为变更）"

echo
echo "== 4) gradle.properties =="
# 只匹配**生效的**那一行：文件里有一大段注释在解释"为什么别写它"，
# 直接 grep 关键字会命中注释，于是永远报 warn——假警报比没警报更烦人。
if grep -E '^[^#]*suppressUnsupportedCompileSdk' "$ROOT/gradle.properties" 2>/dev/null; then
    warn "写了 android.suppressUnsupportedCompileSdk —— 它只压'AGP 只测到 35'的提醒，"
    echo "         压不掉 AAR 元数据硬校验。别指望它解决这堆报错。"
else
    ok "没有 suppressUnsupportedCompileSdk（压了也没用，反而少一条线索）"
fi

echo
echo "== 5) SDK（没有 ANDROID_HOME 就跳过）=="
SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
if [ -z "$SDK" ] || [ ! -d "$SDK" ]; then
    echo "  skip  没设 ANDROID_HOME，跳过平台检查"
    echo "        （CI 的 apk job 会自己装 platforms;android-37）"
else
    echo "  SDK = $SDK"
    ls -d "$SDK"/platforms/android-37* >/dev/null 2>&1 \
        && ok "android-37 平台已装" \
        || bad "缺 android-37 平台：sdkmanager --install \"platforms;android-37\""
    ls -d "$SDK"/build-tools/36.0.0 >/dev/null 2>&1 \
        && ok "build-tools 36.0.0 已装" \
        || warn "缺 build-tools 36.0.0（AGP 9.1.1 的默认版本）"
    if ls -d "$SDK"/platforms/android-37.* >/dev/null 2>&1; then
        warn "android-37 只以次要版本形式存在（android-37.x）。"
        echo "        若报 Failed to find target with hash string 'android-37'，"
        echo "        在 android{} 里补一行 compileSdkMinor = 0（要求 AGP ≥ 9.1.0）。"
    fi
fi

echo
if [ "$fails" = "0" ]; then
    echo "结果：PASS（warn=$warns）。可以开编了。"
    echo "      gradle :app:assembleDebug"
else
    echo "结果：FAIL=$fails（warn=$warns）。"
    echo "      修掉 FAIL 再编，否则会在 checkDebugAarMetadata 撞一整墙报错。"
fi
exit "$fails"
