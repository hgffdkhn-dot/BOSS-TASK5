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
# 用法：
#   bash tools/check_android_toolchain.sh          只检查
#   bash tools/check_android_toolchain.sh --fix     检查并自动清掉残留的旧图标
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

# --fix：自动清掉残留的旧图标文件。
#   为什么要有它：这类文件不在任何引用链上，Manifest 看着完全正常，
#   手工排查永远发现不了它——但 aapt 照样编译它，资源链接照样失败。
#   让脚本自己删，比让人去猜"到底哪个文件没同步"可靠。
FIX=0
[ "${1:-}" = "--fix" ] && FIX=1

# --fix 时**先**清残留，再扫描。
#   反过来的话 3.7 会先报 FAIL、4.8 才删掉文件——同一轮里前后矛盾，
#   看着像"修了但没修好"，还会让人以为要跑两遍。
if [ "$FIX" = "1" ]; then
    cleaned=""
    for f in drawable/ic_boss.xml drawable/ic_launcher.xml; do
        if [ -f "$ROOT/app/src/main/res/$f" ]; then
            rm -f "$ROOT/app/src/main/res/$f"; cleaned="$cleaned $f"
        fi
    done
    if [ -n "$cleaned" ]; then
        echo "[--fix] 已删除残留旧图标文件：$cleaned"
        echo "        记得 git add -A 把这个删除提交进去。"; echo
    else
        echo "[--fix] 没有残留的旧图标文件"; echo
    fi
fi

fails=0; warns=0
failed_items=""          # 收集失败项，末尾汇总（CI 日志常被截断，只留个数字等于白跑）
ok()   { echo "  ok    $1"; }
bad()  { echo "  FAIL  $1"; fails=$((fails+1)); failed_items="$failed_items\n    - $1"; }
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
echo "== 3.5) AGP 9 的内置 Kotlin（这次就是栽在这）=="
# AGP 9.0 起 Kotlin 编译内置进 AGP，再声明 org.jetbrains.kotlin.android 会直接冲突：
#   InvalidPluginException: applying plugin request [id: 'org.jetbrains.kotlin.android', ...]
#   → The 'org.jetbrains.kotlin.android' plugin is no longer required since AGP 9.0
# 报错只说"应用插件时异常"，不提"不该声明"，只看第一行会往"版本不对"上想。
if [ -n "$agp" ] && ver_ge "$agp" "9.0.0"; then
    echo "  AGP $agp ≥ 9 → 内置 Kotlin 生效，kotlin.android 必须不存在"
    hit=0
    for f in build.gradle.kts app/build.gradle.kts gradle/libs.versions.toml; do
        [ -f "$ROOT/$f" ] || continue
        # 只看生效行：文件里有大段注释在解释"为什么没有它"，直接 grep 会误报
        if grep -E '^[^#/*]*org\.jetbrains\.kotlin\.android' "$ROOT/$f" >/dev/null 2>&1; then
            bad "$f 里仍声明了 org.jetbrains.kotlin.android —— AGP 9 下必然冲突，删掉"
            hit=1
        fi
    done
    [ "$hit" = "0" ] && ok "三处构建脚本里都没有 kotlin.android"

    grep -q 'org.jetbrains.kotlin.plugin.compose' "$ROOT/gradle/libs.versions.toml" \
        && ok "Compose 编译器插件在（Kotlin 版本只喂给它）" \
        || bad "缺 org.jetbrains.kotlin.plugin.compose —— @Composable 编不了"

    if grep -q 'kotlin-gradle-plugin:' "$ROOT/build.gradle.kts" 2>/dev/null; then
        kgp=$(grep -oE 'kotlin-gradle-plugin:[0-9]+\.[0-9]+\.[0-9]+' "$ROOT/build.gradle.kts" | head -1 | cut -d: -f2)
        echo "  根脚本 KGP = ${kgp:-未读到}"
        [ "$kgp" = "$kotlin" ] \
            && ok "根脚本 KGP($kgp) == 版本目录 kotlin($kotlin)（两处必须同号）" \
            || bad "根脚本 KGP($kgp) ≠ 版本目录 kotlin($kotlin)。buildscript 里读不到版本目录，得手写同一份"
    else
        warn "根脚本没有 buildscript 拉 KGP —— Kotlin 会被钉在 AGP 内置的 2.2.10"
    fi
else
    echo "  skip  AGP $agp < 9，不适用内置 Kotlin"
fi

echo
echo "== 3.7) 资源里的主题属性引用（launcher 图标专属雷区）=="
# `?attr/xxx`（不带 android: 前缀）在我们自己的包名下解析。
# 它要求 xxx 由某个库（AppCompat / Material Components）或我们自己的
# res/values 声明出来。纯 Compose 项目两个都没有，于是直接链接失败：
#   错误：找不到资源 attr/colorControlNormal
#
# 就算换成 ?android:attr/xxx（框架属性，能编过）也**不能用在 launcher 图标上**——
# 图标是在 launcher 进程里加载的，用 launcher 的 theme 解析，
# 不由我们决定。所以图标里一律写死颜色。
res_dir="$ROOT/app/src/main/res"
if [ ! -d "$res_dir" ]; then
    echo "  skip  没有 res 目录"
else
    # 必须先剥掉 XML 注释再扫：
    #   注释里引用了 "?attr/colorControlNormal" 作为反面例子，
    #   不剥掉的话这条检查会永远报 FAIL——假警报比没警报更烦人。
    #   （上一版 gradle.properties 那条也栽过同样的坑。）
    strip_comments() {
        # ⚠️ 这里**不能**退化成 cat。
        #   注释里常常写着反面例子（比如"?attr/ 是 AppCompat 的属性"），
        #   退化成 cat 就会把注释当成真引用，永远误报 FAIL——
        #   这次在 CI 上就吃了一次：runner 里没有 perl，于是假警报。
        #   （同类坑已经在 gradle.properties 那条上栽过一次。）
        #
        #   优先级：python3（runner 一定有）→ perl → **跳过检查并说明**。
        #   剥不掉就别下结论，宁可少查一项，也不要报一个假 FAIL。
        if command -v python3 >/dev/null 2>&1; then
            python3 -c '
import re,sys
try:
    sys.stdout.write(re.sub(r"<!--.*?-->", "", open(sys.argv[1], encoding="utf-8", errors="replace").read(), flags=re.S))
except Exception:
    sys.stdout.write("")
' "$1" 2>/dev/null
        elif command -v perl >/dev/null 2>&1; then
            perl -0777 -pe 's/<!--.*?-->//gs' "$1" 2>/dev/null
        else
            return 1   # 剥不掉：交给调用方跳过
        fi
    }
    # ⚠️ 一定要报出**具体文件和行号**。
    #   只报属性名（"?attr/xxx 未声明"）害人不浅：
    #   上一轮就因为没报文件，我把"某个 res 文件里的真引用"误判成了
    #   "注释里的假警报"，白改一轮还下错结论。
    #   同一种报错，可能是注释、可能是真代码、可能是没同步过去的旧文件——
    #   不给路径就只能猜。
    if command -v python3 >/dev/null 2>&1 || command -v perl >/dev/null 2>&1; then
        hits=$(find "$res_dir" -type f -name '*.xml' 2>/dev/null \
               | while read -r f; do
                     strip_comments "$f" 2>/dev/null | grep -HnE '\?attr/' --label="$f" || true
                 done)
    else
        hits=""
        warn "没有 python3 也没有 perl，剥不掉 XML 注释——跳过这项检查"
        echo "      （宁可少查一项，也不报假 FAIL：注释里的反面例子会被当成真引用）"
    fi
    if [ -z "$hits" ]; then
        ok "res/ 里没有 ?attr/ 引用"
    else
        # ⚠️ 不能用 `echo "$hits" | while ...`：
        #   管道会把 while 放进**子 shell**，里面的 fails=fails+1 出了子 shell 就没了。
        #   后果极坏——明明打印了 FAIL，结尾却报"结果：PASS"，CI 就放过去了。
        #   （刚刚改这行时真的踩了一次：FAIL 打出来了，退出码却是 0。）
        #   用 here-string，while 留在当前 shell 里，计数才准。
        while IFS= read -r line; do
            f=${line%%:*}; rest=${line#*:}; ln=${rest%%:*}; txt=${rest#*:}
            name=$(printf '%s' "$txt" | grep -oE '\?attr/[A-Za-z_][A-Za-z0-9_]*' | head -1)
            name=${name#\?attr/}
            short=${f#$ROOT/}
            if grep -rqE "<attr name=\"$name\"" "$res_dir"/values/ 2>/dev/null; then
                ok "$short:$ln  $name —— 已在 res/values 声明"
            else
                bad "$short:$ln  用了 ?attr/$name，但未声明"
                echo "          纯 Compose 项目里它必然链接失败。两条路："
                echo "            · 在 res/values 里声明这个 attr"
                echo "            · 图标场景直接写死颜色——图标在 launcher 进程加载，"
                echo "              用 ?attr 解析成什么都不由我们决定"
                echo "          若这个文件已不再被 Manifest 引用，直接删掉它。"
            fi
        done <<< "$hits"
    fi
    # launcher 图标必须是自适应图标（API 26+），否则在 Android 8+ 桌面上
    # 不会被遮罩裁切，形状与邻居不一致。
    if grep -q 'android:icon' "$ROOT/app/src/main/AndroidManifest.xml" 2>/dev/null; then
        if grep -qE 'android:icon="@drawable/' "$ROOT/app/src/main/AndroidManifest.xml"; then
            warn "android:icon 指向 @drawable —— Android 8+ 上不会被遮罩裁切，"
            echo "          桌面图标形状会和别人不一致。用 @mipmap/ic_launcher + adaptive-icon。"
        else
            ok "android:icon 指向 @mipmap（自适应图标）"
        fi
    fi
fi

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
echo "== 4.5) Gradle 版本（AGP 9.1.1 的硬下限是 9.3.1）=="
if [ -n "${GRADLE_VER:-}" ]; then
    echo "  GRADLE_VER = $GRADLE_VER（来自环境）"
    ver_ge "$GRADLE_VER" "9.3.1" \
        && ok "Gradle $GRADLE_VER ≥ 9.3.1" \
        || bad "Gradle $GRADLE_VER < 9.3.1 —— AGP 9.1.1 的硬下限，sync 阶段就会挂"
else
    echo "  skip  没设 GRADLE_VER（CI 会传进来；本地若用 wrapper 可忽略）"
fi

echo
echo "== 4.8) 有没有残留的旧图标文件 =="
# 图标从 @drawable/ic_boss 换成 @mipmap/ic_launcher 之后，
# 旧文件如果不删，它仍然会被 aapt 编译进 APK——
# 于是"Manifest 已经改对了、自检也报 ok"但资源链接照样失败。
# 而且旧文件不在任何引用链上，看 Manifest 永远发现不了它。
stale=""
for f in drawable/ic_boss.xml drawable/ic_launcher.xml; do
    [ -f "$ROOT/app/src/main/res/$f" ] && stale="$stale $f"
done
if [ -n "$stale" ]; then
    if [ "$FIX" = "1" ]; then
        # 理论上到不了这里：--fix 已在脚本开头清过一遍
        ok "仍有残留（--fix 已尝试清理，可能被占用）：$stale"
    else
        bad "残留旧图标文件：$stale"
        echo "      这些文件已不被 Manifest 引用，但仍会被编译。两条路："
        echo "        · bash tools/check_android_toolchain.sh --fix   （自动删）"
        echo "        · rm app/src/main/res/drawable/ic_boss.xml      （手动删）"
    fi
else
    ok "没有残留的旧图标文件"
fi
# 顺带确认所有 drawable 都能被解析到（防止引用了已删掉的东西）
if [ -f "$ROOT/app/src/main/AndroidManifest.xml" ]; then
    miss=""
    while IFS= read -r ref; do
        [ -z "$ref" ] && continue
        d=$(printf '%s' "$ref" | sed 's#@\([a-z]*\)/##')
        # @mipmap 在 mipmap-anydpi-v26，@drawable 在 drawable/
        case "$ref" in
            @mipmap/*) [ -e "$ROOT/app/src/main/res/mipmap-anydpi-v26/$d.xml" ] || miss="$miss $ref" ;;
            @drawable/*) [ -e "$ROOT/app/src/main/res/drawable/$d.xml" ] || miss="$miss $ref" ;;
        esac
    done <<< "$(grep -oE '@(mipmap|drawable)/[A-Za-z0-9_]+' "$ROOT/app/src/main/AndroidManifest.xml")"
    [ -z "$miss" ] && ok "Manifest 引用的图标都存在" || bad "Manifest 引用了不存在的文件：$miss"
fi

echo
echo "== 4.9) veritpath 对接（JNI 符号族 / vendored 源码）=="
# 这一组是任务7 新加的。三条都是"编译链接打包全绿、只在运行时炸"的类型，
# 所以在开编之前静态查一次最划算。
VP_DIR="$ROOT/app/src/main/cpp/veritpath"
VP_JAVA="$ROOT/app/src/main/java/dev/veritpath/Veritpath.java"
VP_JAVA_LOCAL="$VP_JAVA"
if [ ! -d "$VP_DIR" ] || [ ! -f "$VP_JAVA" ]; then
    warn "没有 vendored veritpath —— 修补页会拿不到 .so"
else
    # ① JNI 符号族：Java 的包名.class 必须和 C 里的 Java_<pkg>_<cls>_<method> 对上。
    #    把 Veritpath.java 挪到 com.boss.manager 下是很自然的整理动作，
    #    但改完 C 侧不跟着改，编译、链接、打包三步全绿，
    #    只在 System.loadLibrary 之后抛 UnsatisfiedLinkError。
    jp=$(grep -m1 -E '^package ' "$VP_JAVA" | sed 's/package \(.*\);/\1/')
    want="Java_$(printf '%s' "$jp" | tr '.' '_')_Veritpath"
    got=$(grep -ohE 'Java_[A-Za-z0-9_]+_native[A-Za-z]*' "$VP_DIR/veritpath_jni.c" | head -1)
    case "$got" in
        "$want"*) ok "JNI 符号族对得上（$jp.Veritpath ↔ ${got%_native*}）" ;;
        *) bad "JNI 符号族不匹配：Java 包是 $jp，C 侧却是 ${got%_native*}"
           echo "          两者必须一致，否则运行时 UnsatisfiedLinkError（编译不会报错）" ;;
    esac

    # ② VP_NO_MAIN：不定义它，CLI 的 main() 会被链进 .so。
    grep -q 'VP_NO_MAIN' "$VP_DIR/CMakeLists.txt" \
        && ok "CMake 里定义了 VP_NO_MAIN（不会把 CLI 的 main 链进 .so）" \
        || bad "CMake 缺 VP_NO_MAIN —— .so 里会多一个 main 符号"

    # ③ vendored 源码完整性：漏拷一个 .c 的后果是链接期 undefined reference，
    #    但如果漏的是 vp.h 里声明的东西，现象会更怪。列出来比对最稳。
    n_c=$(ls "$VP_DIR"/src/*.c 2>/dev/null | wc -l)
    if [ "$n_c" -lt 9 ]; then
        bad "vendored 源码只有 $n_c 个 .c（上游是 9 个）—— 复制时漏了"
    else
        ok "vendored 源码 $n_c 个 .c + vp.h"
    fi
    vpv=$(grep -hoE 'VP_VERSION "[0-9.]+"' "$VP_DIR"/src/vp.h 2>/dev/null | head -1)
    echo "  ->   上游版本：${vpv:-未读到}（上游更新后这里的数字会变，可作为漂移信号）"

    # ④ 输出捕获：上游第三次改成了 **pipe 排空到内存**，不碰文件系统。
    #    所以既不需要 BOSS-PATCH，也不再依赖任何可写目录。
    #    （历史：stdout-only → mkstemp 找目录 → pipe。三次现象一样、根因不同。）
    if grep -q 'BOSS-PATCH' "$VP_DIR"/src/util.c 2>/dev/null; then
        bad "util.c 里还留着 BOSS-PATCH —— 上游已自带捕获，请删掉本地补丁"
        echo "          留着会和上游的 dup2 抢同一个 fd，反而把输出搞乱。"
    elif grep -q 'pipe(fds)' "$VP_DIR"/src/util.c 2>/dev/null; then
        ok "捕获已用 pipe（不依赖可写目录），无需本地补丁"
        if grep -q 'g_saved_err' "$VP_DIR"/src/util.c 2>/dev/null; then
            ok "stderr 也在捕获范围内"
        else
            bad "只捕获了 stdout —— 失败时输出会为空"
        fi
    elif grep -q 'g_saved_err' "$VP_DIR"/src/util.c 2>/dev/null; then
        warn "捕获走文件而非 pipe（上游旧版）——确认 setTempDir 已接上"
    else
        bad "util.c 里既没有 pipe 也没有 g_saved_err —— 输出捕获有问题"
        echo "          后果：失败时输出为空，界面只剩'退出码 N'。"
    fi

    # ⑤ setTempDir：现在**可选**。上游换 pipe 后 Java 注释明说
    #   "You do not need this on Android or Linux"。
    #   留着无害（只在 pipe() 都失败时才用的兜底），没有也不是错。
    if grep -rq 'setTempDir' "$ROOT"/app/src/main/java/com/boss/manager/ 2>/dev/null; then
        ok "已调用 Veritpath.setTempDir（现为可选兜底，无害）"
    else
        echo "  ->   没调 setTempDir —— 上游用 pipe 后不需要，可忽略"
    fi

    # ⑥ argv[0] 守卫：上游已修好拼装顺序（head 在前）并在 run() 里加了
    #    显式的"子命令必须在 args[0]"检查，native 侧也有对应诊断。
    #    这里查守卫还在不在——它决定了这类错误是"说清楚的报错"
    #    还是"unknown command"那种莫名其妙的失败。
    if grep -q "sub-command must be args" "$VP_JAVA" 2>/dev/null; then
        ok "上游 run() 有 argv[0] 守卫（拼装错了会明确报错）"
    else
        warn "上游 run() 缺 argv[0] 守卫 —— 拼装错了只会得到 unknown command"
    fi

    # ⑥.5 vendored 的 Veritpath.java 语法对不对？
    #     上游改动这个文件是常事（几乎每次同步都会变），一旦它自己带个语法错，
    #     后果是**整包编译失败**，而报错指向的是上游文件——很容易误判成我们改坏了。
    #     实测踩过一次：上游 run() 的新异常消息里字符串引号没转义。
    if python3 -c "import javalang" 2>/dev/null && [ -f "$VP_JAVA_LOCAL" ]; then
        if python3 -c "
import sys, javalang
javalang.parse.parse(open(sys.argv[1]).read())
" "$VP_JAVA_LOCAL" 2>/dev/null; then
            ok "vendored Veritpath.java 语法 OK"
        else
            bad "vendored Veritpath.java 有语法错误（多半来自上游，需本地转义修复）"
        fi
    fi

    # ⑥.6 上游 2026-10-04 已自己修好字符串转义，本地补丁**已退休**。
    #     不再检查"本地转义还在不在"——那种检查只在补丁期有意义，
    #     留着会在将来某次同步后误报（上游改了措辞就不是那个锚点了）。
    #     语法正确性由上面 ⑥.5 的 javalang 解析兜住，它对任何改动都有效。

    # ⑦ --keep-trailing 接上没有？
    #    dd 出来的整分区镜像（100MB+）repack 后只剩真实内容（几十 MB），
    #    用户会以为修补坏了。上游有这个 flag，但 Java 侧没做专门 API，
    #    要靠 inject(...) 的 extraArgs 传——所以很容易漏接。
    if grep -q 'keep.trailing' "$VP_DIR"/src/main.c 2>/dev/null; then
        if grep -rq 'keep-trailing\|keepTrailing' "$ROOT"/app/src/main/java/com/boss/manager/ 2>/dev/null; then
            ok "已对接 --keep-trailing（大镜像不会莫名变小）"
        else
            bad "上游有 --keep-trailing 但没接 —— dd 出来的大镜像修补后会骤降"
            echo "          表现：100MB+ 进去、几十 MB 出来。不是 bug，但不说明会让人以为坏了。"
        fi
    fi
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
    # 次要版本：**列出来**，而不是笼统提示。
    #   android-37 常常只以 android-37.0 / 37.1 的形式发行。
    #   AGP 找的是精确 hash：compileSdkMinor=0 → android-37.0，=1 → android-37.1。
    #   装了 37.1 却写 0，报的是同一句 "Failed to find target with hash string ..."，
    #   很容易被误判成"平台没装上"——其实装了，只是号码不对。
    minors=$(ls -d "$SDK"/platforms/android-37.* 2>/dev/null \
             | sed 's#.*/android-37\.##' | sort -n | tr '\n' ' ')
    if [ -n "$minors" ]; then
        echo "  ->   已装的 37 次要版本：${minors}"
        first=$(echo "$minors" | awk '{print $1}')
        cur=$(grep -m1 -E '^ *compileSdkMinor *=' "$ROOT/app/build.gradle.kts" 2>/dev/null \
              | grep -oE '[0-9]+')
        if [ -z "$cur" ]; then
            warn "没写 compileSdkMinor，但平台只有 android-37.x ——"
            echo "        AGP 会去找不存在的 'android-37' 并报 Failed to find target。"
            echo "        在 android{} 里加：compileSdkMinor = $first"
        elif [ "$cur" != "$first" ]; then
            warn "compileSdkMinor=$cur，但装的是 android-37.$first —— 号码不一致，"
            echo "        同样会报 Failed to find target。改成 $first。"
        else
            ok "compileSdkMinor=$cur 与已装的 android-37.$cur 对得上"
        fi
    fi
fi

echo
if [ "$fails" = "0" ]; then
    echo "结果：PASS（warn=$warns）。可以开编了。"
    echo "      gradle :app:assembleDebug"
else
    echo "结果：FAIL=$fails（warn=$warns）。"
    echo
    # 汇总放在最后：CI 日志动辄几百行，失败项散在中间很容易被截断掉，
    # 只剩一句 "结果：FAIL=1" 却不知道是哪一项——那就等于白跑。
    # （这次就吃了一次这个亏：只看到 FAIL=1，看不到是谁 FAIL。）
    echo "失败项："
    printf "$failed_items\n"
    echo
    echo "修掉这些再编，否则会在 checkDebugAarMetadata / processDebugResources 撞一整墙报错。"
fi
exit "$fails"
