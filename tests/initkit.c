/* initkit — 直接调 boss_init_main 的测试小工具（不进主构建）
 *
 * 为什么需要它：`init` 这个 applet 标了 needs_root=1，非 root 下 applet 分发
 * 会在**调用 fn 之前**就返回 1。而 GitHub runner 是非 root，于是所有 init
 * 相关的不变量在 CI 上一个都验不到（表现为本地 root 全绿、CI 全红——
 * 接力须知坑 7 的原话）。
 *
 * 但 init 里真正需要特权的只有 mount 一件事，其余逻辑（参数转发、变砖保护、
 * dry run）**根本不需要 root**。needs_root 是给真机 rc 脚本用的正确语义，
 * 不该为了让测试跑起来就去动它——那就把测试从 applet 层绕过去。
 *
 * 做法与 tests/sepolkit.c 完全一致：单独编一个只调目标函数的二进制。
 *
 * 用法：./build/initkit hijack-prep --dry   等价于 boss init hijack-prep --dry
 */
#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/boss.h"

int main(int argc, char **argv)
{
    /* 补一个 argv[0]="init"：boss_init_main 从 argv[1] 取子命令，
     * 与 applet 分发（symlink 调用）传给它的形态完全一致。 */
    char **a = calloc((size_t)argc + 1, sizeof(char *));
    if (!a) return 1;
    a[0] = (char *)"init";
    for (int i = 1; i < argc; i++) a[i] = argv[i];

    int rc = boss_init_main(argc, a);
    free(a);
    return rc;
}
