/* BOSS · App 侧 IPC 客户端（任务6）
 *
 * 这一层是 BOSS App 与 bossd 之间的**唯一**通道。它必须不长出特权：
 *   · App 进程拿不到任何 setuid / capabilities，它只是"管道"；
 *   · 身份由内核 SO_PEERCRED 提供，App 无法伪造（架构红线第 2 条）；
 *   · 授权判定、fork、pty、切身份、SELinux 切换全部在 daemon 侧。
 *
 * 协议与 src/boss.h 的 struct boss_request 一一对应。
 * ⚠️ 改 src/boss.h 必须同步改这里，并且**升 BOSS_PROTO_VER**
 *    （任务2 交接时的原话，仍然有效）——否则新旧二进制静默错位。
 *    tools/ipc_layout_test.c 会在编译期把两边 sizeof / 字段偏移钉死，
 *    CI 上这一步红了就是真的错位了。
 *
 * 握手顺序是硬要求（接力须知坑 4.1）：建连后第一件事必须是 SCM_RIGHTS
 * 握手，先把"退出码回传通道"交出去。否则 daemon 的 recvmsg 会把请求体的
 * 第一个字节当成握手数据吃掉，后面整条流全部错位——表现为"ping 永远超
 * 时、其它命令时好时坏"。
 */
#ifndef BOSS_IPC_H
#define BOSS_IPC_H

#include <stdint.h>
#include <stddef.h>

/* 与 src/boss.h 保持一致；ipc_layout_test.c 会做静态断言 */
#define BOSS_IPC_MAGIC      0xB0550001u
#define BOSS_IPC_PROTO_VER  2u

#define BOSS_IPC_MAX_SHELL   64
#define BOSS_IPC_MAX_CMD     1024
#define BOSS_IPC_MAX_CTX     64

/* 响应码（与 boss.h 的 BOSS_OK/DENIED/ERR/PROMPT 对齐） */
#define BOSS_IPC_OK     0u
#define BOSS_IPC_DENIED 1u
#define BOSS_IPC_ERR    2u
#define BOSS_IPC_PROMPT 3u

/* 请求标志位（与 boss.h 对齐，这里只声明 App 会用到的） */
#define BOSS_IPC_F_LOGIN    (1u << 0)
#define BOSS_IPC_F_KEEPENV  (1u << 1)
#define BOSS_IPC_F_NOLOG    (1u << 2)   /* 本次请求不进审计日志 */
#define BOSS_IPC_F_PING     (1u << 3)
/* UI 控制通道（任务6 新增）：不 fork 子进程，command 是控制指令。
 * 老 daemon（proto v1）不认这个标志会把指令当 shell 命令跑一遍，
 * 所以协议版本同时升到 2——语义变了就必须升，不能让它静默发生。 */
#define BOSS_IPC_F_UI       (1u << 4)

/* 结构体布局必须与 src/boss.h 的 struct boss_request 完全一致。
 * 不要为了"好看"调字段顺序——那是二进制协议，不是普通结构体。 */
struct boss_ipc_request {
    uint32_t magic;
    uint32_t version;
    uint32_t target_uid;
    uint32_t target_gid;
    uint32_t flags;
    uint32_t env_len;
    uint32_t arg_len;
    uint16_t rows;
    uint16_t cols;
    char     shell[BOSS_IPC_MAX_SHELL];
    char     command[BOSS_IPC_MAX_CMD];
    char     context[BOSS_IPC_MAX_CTX];
};

struct boss_ipc_response {
    uint32_t code;
    uint32_t target_uid;
};

/* 执行结果。out 是 malloc 出来的，调用方 free。 */
struct boss_ipc_result {
    uint32_t code;        /* BOSS_IPC_OK / DENIED / ERR / PROMPT */
    int      exit_code;   /* 子进程的退出码；未取到为 -1 */
    int      timed_out;   /* 1 = 超时被我们主动掐断（输出可能不完整） */
    char    *out;         /* 子进程输出（pty 过来的，CRLF 已归一为 LF） */
    size_t   out_len;
};

/* 单次执行的可选参数。不填的用 0 / NULL，内部给默认值。 */
struct boss_ipc_opts {
    const char *command;   /* -c 的内容；NULL = 交互式（App 别用） */
    const char *shell;     /* NULL = daemon 默认 shell */
    const char *context;   /* 目标 SELinux domain；NULL = 默认 */
    uint32_t    uid;       /* 目标 uid，默认 0 */
    uint32_t    gid;       /* 目标 gid，默认 0 */
    uint32_t    flags;     /* BOSS_IPC_F_* */
    int         timeout_ms;/* 整体超时；<=0 用默认 15000 */
    size_t      max_out;   /* 输出上限；0 = 默认 1MB */
};

/* 探活。返回 0 = bossd 在，1 = 不在（连不上），-1 = 协议错/被拒。 */
int boss_ipc_ping(void);

/* 执行一条命令。
 * 返回 0 表示"跑到了子进程"（结果看 res->code 与 res->exit_code）；
 * 返回 -1 表示连不上 daemon 或本地参数错（此时 res->out 为 NULL）。
 * 注意：res->code == BOSS_IPC_DENIED 不是函数失败，是策略拒绝——
 *       App 要把它显示成"被拒绝"，而不是"出错了"。 */
int boss_ipc_run(const struct boss_ipc_opts *opts, struct boss_ipc_result *res);

void boss_ipc_result_free(struct boss_ipc_result *res);

/* 把 pty 输出的 CRLF 归一成 LF，并去掉尾部的空行。
 * 单独暴露出来是因为 App 的解析器要按行切，CRLF 会让每行末尾多个 \r，
 * 表现为"字符串相等判断永远失败"——和 resetprop 那个坑同类。 */
void boss_ipc_normalize(char *buf, size_t *len);

#endif /* BOSS_IPC_H */
