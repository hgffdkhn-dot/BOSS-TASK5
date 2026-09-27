/* BOSS · 协议布局自检（任务6）
 *
 * 为什么要有这么一个"什么都不做"的程序：
 *   App 的 IPC 客户端在 app/src/main/cpp/boss_ipc.h 里**重新声明**了一遍
 *   struct boss_request（JNI 层拿不到 src/boss.h，也不该把内部头拖进 App）。
 *   一旦上游改了 src/boss.h 而这里没跟上，两边 sizeof 不同，表现不是报错，
 *   而是"命令发出去、daemon 读到一串错位的值"——最坏情况是 target_uid 字段
 *   读到了别的值。这类 bug 在没有真机的时候几乎查不出来。
 *
 * 所以这里在**编译期**把两边钉死：sizeof、每个字段的 offsetof、magic 与
 * 协议版本。CI 上这一步红了，就是真的错位了，千万不要靠"放宽断言"让它绿。
 *
 * 编译（BOSS_SRC 指向 boss 本体仓库的 src 目录）：
 *   cc -std=c11 -I"$BOSS_SRC" -I../app/src/main/cpp \
 *      -o build/ipc_layout_test tools/ipc_layout_test.c
 */
#include <stddef.h>
#include <stdio.h>

#include "boss.h"          /* 上游：src/boss.h */
#include "boss_ipc.h"      /* 本仓库：App 侧的镜像声明 */

/* 字段级断言：只比对 sizeof 不够——两个字段换了顺序也照样 sizeof 相等。 */
#define ASSERT_OFF(real, mirror, field) \
    _Static_assert(offsetof(struct real, field) == offsetof(struct mirror, field), \
                   "field offset mismatch: " #field)

_Static_assert(sizeof(struct boss_request) == sizeof(struct boss_ipc_request),
               "struct boss_request size mismatch");
_Static_assert(sizeof(struct boss_response) == sizeof(struct boss_ipc_response),
               "struct boss_response size mismatch");

ASSERT_OFF(boss_request, boss_ipc_request, magic);
ASSERT_OFF(boss_request, boss_ipc_request, version);
ASSERT_OFF(boss_request, boss_ipc_request, target_uid);
ASSERT_OFF(boss_request, boss_ipc_request, target_gid);
ASSERT_OFF(boss_request, boss_ipc_request, flags);
ASSERT_OFF(boss_request, boss_ipc_request, env_len);
ASSERT_OFF(boss_request, boss_ipc_request, arg_len);
ASSERT_OFF(boss_request, boss_ipc_request, rows);
ASSERT_OFF(boss_request, boss_ipc_request, cols);
ASSERT_OFF(boss_request, boss_ipc_request, shell);
ASSERT_OFF(boss_request, boss_ipc_request, command);
ASSERT_OFF(boss_request, boss_ipc_request, context);

_Static_assert(BOSS_MAGIC == BOSS_IPC_MAGIC, "magic mismatch");
/* 断言消息一律用 ASCII：某些编译器会把字符串里的非 ASCII 打成八进制转义
 * （\37777777746 那一串），错误信息直接变得没法读——真出事的时候最需要看清的就是它。 */
_Static_assert(BOSS_PROTO_VER == BOSS_IPC_PROTO_VER,
               "proto version mismatch: bump BOTH src/boss.h and "
               "app/src/main/cpp/boss_ipc.h (or the task6 patch is NOT applied)");

/* 响应码与标志位：语义错位比布局错位更难查（不崩，只是行为不对）。 */
_Static_assert(BOSS_OK == BOSS_IPC_OK, "BOSS_OK mismatch");
_Static_assert(BOSS_DENIED == BOSS_IPC_DENIED, "BOSS_DENIED mismatch");
_Static_assert(BOSS_ERR == BOSS_IPC_ERR, "BOSS_ERR mismatch");
_Static_assert(BOSS_PROMPT == BOSS_IPC_PROMPT, "BOSS_PROMPT mismatch");
_Static_assert(BOSS_F_PING == BOSS_IPC_F_PING, "BOSS_F_PING mismatch");
_Static_assert(BOSS_F_NOLOG == BOSS_IPC_F_NOLOG, "BOSS_F_NOLOG mismatch");
_Static_assert(BOSS_F_LOGIN == BOSS_IPC_F_LOGIN, "BOSS_F_LOGIN mismatch");
_Static_assert(BOSS_F_KEEPENV == BOSS_IPC_F_KEEPENV, "BOSS_F_KEEPENV mismatch");

int main(void)
{
    printf("PASS 协议布局一致：sizeof(request)=%zu sizeof(response)=%zu proto=v%u\n",
           sizeof(struct boss_request), sizeof(struct boss_response), BOSS_PROTO_VER);
    return 0;
}
