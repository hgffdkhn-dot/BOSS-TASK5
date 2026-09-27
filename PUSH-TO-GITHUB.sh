#!/bin/bash
# BOSS-TASK5 建仓并推送 —— 给主办方执行
#
# 背景：我这边网络出口对 GitHub 写入受限（只能下载、推不上去），
# 所以本地已经把仓库初始化好、commit 打好、tag 打好了，
# 这里只差"建一个空仓库 + 推上去"这两步，必须由你们来跑。
#
# 用法：
#   bash PUSH-TO-GITHUB.sh
# 或先在 GitHub 网页上建好空仓库后手工执行文末那两行。
set -eu
cd "$(dirname "$0")"

REPO_OWNER="hgffdkhn-dot"
REPO_NAME="BOSS-TASK5"

echo "目标仓库：${REPO_OWNER}/${REPO_NAME}.git"
echo

# 0) 自检：脚本千万别漏（.gitignore 注释里警告过，本次收尾时真的踩了一次）
if [ ! -f build/build-ndk.sh ]; then
    echo "ERROR: build/build-ndk.sh 不见了。CI 的 android job 第一步就调它，" >&2
    echo "       而本地永远发现不了（脚本一直躺在自己的工作区里）。" >&2
    echo "       请从 BOSS-TASK4 仓库取回后再执行本脚本。" >&2
    exit 1
fi
echo "[ok] build/build-ndk.sh 在位"

# 1) 本地再跑一遍门禁，别把红的推上去
echo
echo "== 构建门禁 =="
make clean >/dev/null 2>&1
make test >/dev/null 2>&1 || { echo "构建失败"; exit 1; }
./build/boss -V
echo "[ok] 构建通过"

# 2) 建空仓库（需要 gh 已登录；没装 gh 就跳过，改在网页上建）
echo
echo "== 建空仓库 =="
if command -v gh >/dev/null 2>&1; then
    if gh repo view "${REPO_OWNER}/${REPO_NAME}" >/dev/null 2>&1; then
        echo "[skip] 仓库已存在"
    else
        gh repo create "${REPO_OWNER}/${REPO_NAME}" --public \
            --description "BOSS · 任务5：无修改系统逻辑与特典逻辑" || {
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
if git remote get-url origin >/dev/null 2>&1; then
    git remote set-url origin "https://github.com/${REPO_OWNER}/${REPO_NAME}.git"
else
    git remote add origin "https://github.com/${REPO_OWNER}/${REPO_NAME}.git"
fi
git branch -M main
git push -u origin main
echo "[ok] main 已推送"

# 4) tag（会触发 release.yml 出包：Android 四 ABI + 静态 + payload zip）
echo
echo "== 打 tag 出包 =="
# 出包依赖 NDK 交叉编译，CI 的 android job 最容易红。
# 想先看 CI 结果再出包的话，这一步可以缓一缓。
git push origin v0.2.0
echo "[ok] v0.2.0 已推送，release 工作流开始出包"

echo
echo "完成。接下来应该关注的两个 job："
echo "  · task5.yml  —— 任务5 三套验收（systemless 16 / hide 17 / hijack 9）"
echo "  · release    —— v0.2.0 出包（android job 红了就是真的有问题，别放宽校验）"
echo
echo "真机 bring-up 第一件事：ls -l /proc/1/exe 指不指向 boss。"
echo "务必带 boss_selinux=0 自救路径再上机。"
