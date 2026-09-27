/* BOSS · IPC 探针（任务6，测试用）
 *
 * 一个极小的命令行壳，套在 boss_ipc 之上，让 shell 脚本能验两件事：
 *   ipc_probe ui  <op>     UI 控制通道（manager / pending / allow <id> / deny <id>）
 *   ipc_probe run <cmd>    普通执行通道
 *
 * 它为什么存在：manager 身份的判定要看**内核给的 uid** 与 zygote 填的
 * cmdline（/proc/<pid>/cmdline）。脚本里要用 `exec -a com.boss.manager` 把
 * argv[0] 伪装成 App 进程，才能验到"抢注 / 自动放行"这条路径——
 * 真机上这一步由 zygote 替我们填，沙盒里只能自己演一遍。
 */
#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boss_ipc.h"

static void usage(const char *me)
{
    fprintf(stderr, "用法: %s ui <op> | run <cmd> | selftest\n", me);
}

int main(int argc, char **argv)
{
    /* 自举：不连 daemon，只证明"这个二进制能被目标 uid 真正 exec 起来"。
     *
     * 为什么需要它：CI 上出现过——探针编译成功了，但被 setpriv 切成别的 uid
     * 之后 exec 报 `cannot execute: Permission denied`（多半是 umask/权限位，
     * 或工作区挂载带 noexec）。那种情况下后面 9 个用例会全部 FAIL，
     * 而真实原因只是"二进制跑不起来"——又是那种指向错误方向的报错。
     * 所以脚本在正式开跑之前，先用每个目标 uid 空跑一次这个模式。 */
    if (argc >= 2 && strcmp(argv[1], "selftest") == 0) {
        printf("code=SELFTEST exit=0 timeout=0\n");
        printf("--- output ---\nprobe-ok\n");
        return 0;
    }

    if (argc < 3) { usage(argv[0]); return 2; }

    struct boss_ipc_opts o;
    memset(&o, 0, sizeof(o));
    o.timeout_ms = 70000;   /* 弹窗等待最长 60s + 余量 */

    /* 把 argv[2..] 拼回一条命令：shell 里 `probe run echo hello` 会被切成
     * 四个参数，只取 argv[2] 的话传到 daemon 的命令是 "echo"，
     * 输出自然是空的——症状看起来像"IPC 坏了"，其实是探针自己的锅。 */
    static char cmd[1024];
    size_t off = 0;
    for (int i = 2; i < argc && off + 1 < sizeof(cmd); i++) {
        if (i > 2 && off + 1 < sizeof(cmd)) cmd[off++] = ' ';
        size_t n = strlen(argv[i]);
        if (off + n >= sizeof(cmd)) n = sizeof(cmd) - off - 1;
        memcpy(cmd + off, argv[i], n);
        off += n;
        cmd[off] = '\0';
    }

    int is_ui = strcmp(argv[1], "ui") == 0;
    if (is_ui) {
        o.flags = BOSS_IPC_F_UI | BOSS_IPC_F_NOLOG;
        o.command = cmd;
    } else if (strcmp(argv[1], "run") == 0) {
        o.command = cmd;
    } else {
        usage(argv[0]);
        return 2;
    }

    struct boss_ipc_result r;
    memset(&r, 0, sizeof(r));
    int rc = boss_ipc_run(&o, &r);
    if (rc < 0) {
        printf("code=DOWN exit=-1\n");
        printf("--- output ---\n\n");
        return 1;
    }
    printf("code=%u exit=%d timeout=%d\n", r.code, r.exit_code, r.timed_out);
    printf("--- output ---\n%s\n", r.out ? r.out : "");
    boss_ipc_result_free(&r);
    return 0;
}
