#!/bin/bash
# BOSS · 全量包建仓并推送 —— 给主办方执行
#
# 背景：我这边网络出口对 GitHub 写入受限（只能下载、推不上去），
# 所以"建空仓库 + 推上去"这两步必须由你们来跑。
#
# ⚠️ 这个脚本覆盖的是**全量包**（BOSS-TASK5 的全部内容 + 任务6 的增量）。
#    我一份前辈的文件都没删，只做增量添加与修改：
#      · 改：src/boss.h、src/daemon.c、src/policy.c、Makefile
#      · 加：src/manager.c、src/prompt.c、app/、tools/任务6 四套、docs/TASK6
#    所以把它推到 BOSS-TASK5（覆盖式）在内容上是安全的——它本来就是
#    BOSS-TASK5 + 任务6。
#
# 两种推法，二选一（改下面 REPO_NAME 即可）：
#   1) REPO_NAME="BOSS-TASK5"  → 覆盖前辈存档，当全量包使（推荐：单一事实来源）
#   2) REPO_NAME="BOSS-TASK6"  → 新建仓库，前辈的 BOSS-TASK5 原样保留
#
# 用法：
#   bash PUSH-TO-GITHUB.sh
# 或先在 GitHub 网页上建好空仓库后手工执行文末那几行。
set -eu
cd "$(dirname "$0")"

REPO_OWNER="hgffdkhn-dot"
REPO_NAME="${REPO_NAME:-BOSS-TASK5}"   # 想建新仓就：REPO_NAME=BOSS-TASK6 bash PUSH-TO-GITHUB.sh

echo "目标仓库：${REPO_OWNER}/${REPO_NAME}.git"
echo "（想建新仓：REPO_NAME=BOSS-TASK6 bash PUSH-TO-GITHUB.sh）"
echo

# 0) 自检：两个"在家但其实容易被整目录忽略"的脚本（.gitignore 注释里警告过，
#    本次收尾时真的踩过一次：rm -rf build 把 build-ndk.sh 一起删了）
for f in build/build-ndk.sh app/src/main/cpp/CMakeLists.txt; do
    if [ ! -f "$f" ]; then
        echo "ERROR: $f 不见了。CI 第一步就调它，" >&2
        echo "       而本地永远发现不了（它一直躺在自己的工作区里）。" >&2
        exit 1
    fi
    echo "[ok] $f 在位"
done

# 1) 本地再跑一遍门禁，别把红的推上去
echo
echo "== 构建门禁 =="
make clean >/dev/null 2>&1
make test >/dev/null 2>&1 || { echo "构建失败"; exit 1; }
./build/boss -V
echo "[ok] 构建通过"

echo
echo "== 任务6 三套自检 =="
bash tools/run_ipc_test.sh      | tail -3
bash tools/manager_flow_test.sh | tail -2
bash tools/contract_test.sh     | tail -1

# 2) 建空仓库（需要 gh 已登录；没装 gh 就跳过，改在网页上建）
echo
echo "== 建空仓库 =="
if command -v gh >/dev/null 2>&1; then
    if gh repo view "${REPO_OWNER}/${REPO_NAME}" >/dev/null 2>&1; then
        echo "[skip] 仓库已存在（覆盖式推送）"
    else
        gh repo create "${REPO_OWNER}/${REPO_NAME}" --public \
            --description "BOSS · 全量包：su + 关键组件 + SELinux + 无修改系统逻辑 + 客户端" || {
            echo "创建失败（多半是没登录：gh auth login）"; exit 1; }
        echo "[ok] 仓库已建"
    fi
else
    echo "[skip] 没装 gh —— 请在 GitHub 网页上手动建一个**空**仓库："
    echo "       ${REPO_OWNER}/${REPO_NAME}（不要勾 README / .gitignore）"
    read -r -p "       建好后按回车继续..." _
fi

# 3) 推送
echo
echo "== 推送 =="
if git rev-parse --git-dir >/dev/null 2>&1; then
    echo "[ok] 已是 git 仓库"
else
    git init -q
    git add -A
    git -c user.name=BOSS -c user.email=boss@local commit -q -m "BOSS 全量包：任务2-6"
fi
if git remote get-url origin >/dev/null 2>&1; then
    git remote set-url origin "https://github.com/${REPO_OWNER}/${REPO_NAME}.git"
else
    git remote add origin "https://github.com/${REPO_OWNER}/${REPO_NAME}.git"
fi
git branch -M main
git add -A
git -c user.name=BOSS -c user.email=boss@local commit -q -m "任务6：BOSS 客户端与功能组件对接" || echo "[skip] 没有新的改动需要提交"
git push -u origin main
echo "[ok] main 已推送"

# 4) tag（会触发 release.yml 出包：Android 四 ABI + 静态 + payload zip）
echo
echo "== 打 tag 出包 =="
# 任务6 把协议升到了 v2，版本号跟着走。
git tag -f v0.3.0 2>/dev/null || true
git push -u origin v0.3.0 2>/dev/null || echo "[skip] tag 推送失败（可稍后在网页上发）"
echo "[ok] v0.3.0 已处理"

echo
echo "完成。接下来应该关注的 job："
echo "  · task6.yml  —— 协议布局断言 / IPC 端到端 / manager+弹窗 / CLI 契约 / 上游回归"
echo "  · release    —— v0.3.0 出包（android job 红了就是真的有问题，别放宽校验）"
echo
echo "真机 bring-up 第一件事：ls -l /proc/1/exe 指不指向 boss。"
echo "务必带 boss_selinux=0 自救路径再上机。"
