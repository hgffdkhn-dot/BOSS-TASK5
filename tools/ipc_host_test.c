/* BOSS · App IPC 链路端到端测试（任务6）
 *
 * 这一段在**主机**上跑真实的 bossd：抽象命名空间套接字、SCM_RIGHTS 握手、
 * 请求体、pty 输出、退出码回传全都是真链路，只有"提权"这一步在真机上才有
 * 意义（daemon 非 root 时会把目标身份降级为自身，见 daemon.c）。
 *
 * 它的价值在于：App 端那套 IPC 代码在拿到真机之前就能被验一遍。
 * 握手顺序、退出码回传、CRLF 归一这三件事写错了在真机上表现为
 * "偶尔卡住 / 退出码永远是 0 / 解析永远不匹配"，都极难定位。
 *
 * 用法：tools/run_ipc_test.sh（会自己起 daemon、跑完再收掉）
 */
#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boss_ipc.h"

static int failures = 0;
static int checks = 0;

static void check(int ok, const char *what)
{
    checks++;
    printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

static int run(const char *cmd, struct boss_ipc_result *res)
{
    struct boss_ipc_opts o;
    memset(&o, 0, sizeof(o));
    o.command = cmd;
    o.timeout_ms = 10000;
    return boss_ipc_run(&o, res);
}

int main(void)
{
    printf("BOSS · IPC 链路测试\n");

    /* 1) 探活 */
    int p = boss_ipc_ping();
    check(p == 0, "boss_ipc_ping 返回 0（bossd 在跑）");

    /* 2) 正常输出 */
    struct boss_ipc_result r;
    memset(&r, 0, sizeof(r));
    int rc = run("echo hello-boss", &r);
    check(rc == 0, "boss_ipc_run 返回 0（连上并拿到响应）");
    check(r.code == BOSS_IPC_OK, "响应码 OK");
    check(r.out && strstr(r.out, "hello-boss") != NULL, "输出里能找到 hello-boss");
    check(r.exit_code == 0, "退出码 0");
    check(r.out && strchr(r.out, '\r') == NULL, "CRLF 已归一（输出里没有裸 \\r）");
    boss_ipc_result_free(&r);

    /* 3) 退出码独立回传（任务2 的老断言：su -c 'exit 7' → 7）
     *    退出码走的是 side channel，不是主数据通道——混在一起的话
     *    退出码会被当成输出打印出来。 */
    memset(&r, 0, sizeof(r));
    rc = run("exit 7", &r);
    check(rc == 0 && r.code == BOSS_IPC_OK, "exit 7 请求被受理");
    check(r.exit_code == 7, "退出码回传为 7");
    boss_ipc_result_free(&r);

    /* 4) 多行输出按行切得开（App 的解析器依赖这一点） */
    memset(&r, 0, sizeof(r));
    rc = run("printf 'a\\nb\\nc\\n'", &r);
    int lines = 0;
    if (r.out) {
        for (char *s = r.out; *s; s++) if (*s == '\n') lines++;
        if (r.out[0] && r.out[strlen(r.out) - 1] != '\n') lines++;
    }
    check(rc == 0 && lines == 3, "三行输出被解析成 3 行（尾部空行已去掉）");
    boss_ipc_result_free(&r);

    /* 5) 大输出不被截断（App 会拿它读日志） */
    memset(&r, 0, sizeof(r));
    rc = run("i=0; while [ $i -lt 400 ]; do echo line-$i; i=$((i+1)); done", &r);
    check(rc == 0 && r.out && strstr(r.out, "line-399") != NULL,
          "400 行输出能完整取回（动态扩容生效）");
    boss_ipc_result_free(&r);

    printf("\n%d 项，失败 %d 项\n", checks, failures);
    return failures ? 1 : 0;
}
