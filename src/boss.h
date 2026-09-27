/* BOSS su — 公共定义
 *
 * 设计要点：
 *  - 守护进程 bossd 常驻，客户端 su 只是"管道"：所有授权判定、子进程创建、
 *    SELinux 上下文切换都在 daemon 侧完成，客户端拿不到任何特权。
 *  - IPC 使用 AF_UNIX 抽象命名空间（名字不落盘，/dev/socket 下无痕迹）。
 *  - 调用者身份由内核 SO_PEERCRED 提供，客户端无法伪造。
 */
#ifndef BOSS_H
#define BOSS_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/types.h>

#define _GNU_SOURCE 1

#define BOSS_MAGIC      0xB0550001u   /* "BOSS" v1 */
#define BOSS_PROTO_VER  2u
#define BOSS_VERSION    "0.1.0"

/* ---- 运行时路径（编译期可覆盖，便于主机侧冒烟测试） ---- */
#ifndef BOSS_DIR
#define BOSS_DIR "/data/adb/boss"
#endif
#define BOSS_LOG_PATH    BOSS_DIR "/boss.log"
#define BOSS_POLICY_PATH BOSS_DIR "/policy.conf"
#define BOSS_BIN_PATH    BOSS_DIR "/boss"
#define BOSS_TMP_DIR     BOSS_DIR "/tmp"

/* ---- 任务3（关键组件）运行时路径 ----
 * 全部落在 BOSS_DIR 下：/system 一个字节都不写（隐蔽红线）。 */
#define BOSS_MODULE_DIR    BOSS_DIR "/modules"      /* 模块根目录 */
#define BOSS_BIN_DIR       BOSS_DIR "/bin"          /* busybox / applet symlink */
#define BOSS_POSTFS_DIR    BOSS_DIR "/post-fs-data.d"
#define BOSS_SERVICE_DIR   BOSS_DIR "/service.d"
#define BOSS_BOOTCOMP_DIR  BOSS_DIR "/boot-completed.d"
#define BOSS_PERSIST_FILE  BOSS_DIR "/persist.props"  /* persist 属性快照（开机重放） */
#define BOSS_MOUNT_LOG     BOSS_DIR "/mount.log"

/* ---- 任务5（无修改系统逻辑 + 特典逻辑）运行时路径 ----
 * 全部落在 BOSS_DIR 下，与任务3 同一条红线：/system 一个字节都不写。 */
#define BOSS_SYLESS_CONF   BOSS_DIR "/systemless.conf"  /* 声明式"系统改动"清单 */
#define BOSS_SYLESS_BASE   BOSS_DIR "/systemless.baseline" /* 只读分区基线快照（verify 用） */
#define BOSS_DENY_CONF     BOSS_DIR "/denylist.conf"    /* 隐藏名单（特典） */
#define BOSS_MANAGER_FILE  BOSS_DIR "/manager.uid"      /* 任务6：BOSS App 的 uid */
#define BOSS_PROMPT_DIR    BOSS_DIR "/prompt"           /* 任务6：待用户裁决的授权请求 */
#define BOSS_DENY_STATE    BOSS_DIR "/hide.state"       /* 已处理的 (pid,starttime)，防重复 umount */
#define BOSS_PROPS_CONF    BOSS_DIR "/props.conf"       /* 属性伪装清单（特典） */

/* 属性区：每块 128KB，8.0+ 按 property_contexts 分成多个区域文件 */
#define BOSS_PROP_DIR      "/dev/__properties__"
#define BOSS_PROP_SERIAL   "properties_serial"      /* 只用来放全局 serial */
#define BOSS_PERSIST_DB    "/data/property/persistent_properties"
#define BOSS_PERSIST_DIR   "/data/property"         /* legacy：一属性一文件 */

/* 抽象命名空间套接字名：文件系统里看不到，符合"低痕迹"目标 */
#define BOSS_SOCK_NAME   "bossd"

#ifdef __ANDROID__
#define BOSS_DEFAULT_SHELL "/system/bin/sh"
#define BOSS_DEFAULT_PATH  "/sbin:/system/sbin:/product/bin:/apex/com.android.runtime/bin:/system/bin:/system/xbin:/vendor/bin:/vendor/xbin"
#define BOSS_DEFAULT_HOME  "/data"
#define BOSS_DEFAULT_TMP   "/data/local/tmp"
#else
#define BOSS_DEFAULT_SHELL "/bin/sh"
#define BOSS_DEFAULT_PATH  "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
#define BOSS_DEFAULT_HOME  "/root"
#define BOSS_DEFAULT_TMP   "/tmp"
#endif

#define BOSS_MAX_SHELL   64
#define BOSS_MAX_CMD     1024
#define BOSS_MAX_CTX     64
#define BOSS_MAX_BLOB    (64u * 1024u)
#define BOSS_MAX_ENVP    128

/* ---- 响应码 ---- */
enum {
    BOSS_OK     = 0,   /* 已授权，后面是数据流 */
    BOSS_DENIED = 1,   /* 策略拒绝 */
    BOSS_ERR    = 2,   /* 协议/内部错误 */
    BOSS_PROMPT = 3    /* 需要前端（BOSS App）弹窗确认；当前实现按拒绝处理 */
};

/* ---- 请求标志位 ---- */
#define BOSS_F_LOGIN    (1u << 0)   /* 登录式 shell（argv[0] 前导 '-'） */
#define BOSS_F_KEEPENV  (1u << 1)   /* 保留调用方环境变量 */
#define BOSS_F_NOLOG    (1u << 2)   /* 本次请求不记日志 */
#define BOSS_F_PING     (1u << 3)   /* 只探活，不执行命令 */
/* 任务6：UI 控制通道。不 fork 子进程，command 字段是控制指令而不是 shell 命令。
 * 老 daemon 不认这个标志会把指令当命令跑一遍——所以协议版本同时升到了 2，
 * 不让它静默发生（"扩展语义必须升 PROTO_VER"是任务2 交接时的原话）。 */
#define BOSS_F_UI       (1u << 4)   /* App 控制通道：pending / allow <id> / deny <id> */

struct boss_request {
    uint32_t magic;
    uint32_t version;
    uint32_t target_uid;
    uint32_t target_gid;
    uint32_t flags;
    uint32_t env_len;
    uint32_t arg_len;
    uint16_t rows;
    uint16_t cols;
    char     shell[BOSS_MAX_SHELL];   /* 空串 = 默认 shell */
    char     command[BOSS_MAX_CMD];   /* -c 的命令；空串 = 交互式 */
    char     context[BOSS_MAX_CTX];   /* 目标 SELinux domain；空串 = 默认 */
};

struct boss_response {
    uint32_t code;
    uint32_t target_uid;
};

/* ---- util.c ---- */
void  boss_log_init(const char *path, int enabled);
void  boss_log(const char *fmt, ...);
/* 直接追加一行到 BOSS_LOG_PATH，不依赖 boss_log_init。
 * 开机早期（日志开关还没读）也要能留下痕迹，排查时才知道卡在哪一步。 */
void  boss_log_line(const char *fmt, ...);
int   boss_write_full(int fd, const void *buf, size_t len);
int   boss_read_full(int fd, void *buf, size_t len);
int   boss_connect(void);                       /* 连到 daemon，返回 fd 或 -1 */
int   boss_peer_uid(int fd, uid_t *uid, pid_t *pid);
size_t boss_blob_add(char *buf, size_t off, size_t cap, const char *s);
void  boss_copy(char *dst, size_t n, const char *src);
int   boss_proc_cmdline(pid_t pid, char *buf, size_t len);
int   boss_send_fd(int sock, int fd);
int   boss_send_handshake(int sock);   /* 建连第一步：送出退出码通道，返回本地端 fd */
int   boss_recv_fd(int sock);
int   boss_mkdirs(const char *path, mode_t mode);
int   boss_daemonize(void);
void  boss_set_log(int on);

/* blob 遍历：[uint32 len][bytes 含结尾 NUL] 重复 */
static inline int boss_blob_next(const char *buf, size_t len, size_t *off,
                                 char *out, size_t outsz)
{
    uint32_t n;
    if (!buf || *off + 4 > len) return 0;
    memcpy(&n, buf + *off, 4);
    *off += 4;
    if (n == 0 || n > BOSS_MAX_BLOB || *off + n > len) return 0;
    if (n > outsz) n = (uint32_t)outsz;
    memcpy(out, buf + *off, n);
    out[n - 1] = '\0';
    *off += n;
    return 1;
}

/* ---- policy.c ---- */
enum { BOSS_DECISION_DENY = 0, BOSS_DECISION_ALLOW = 1, BOSS_DECISION_PROMPT = 2 };

struct boss_rule {
    int type;       /* 0=uid 精确 1=app(appId,跨用户) 2=user(userId) */
    int id;
    int decision;
};

struct boss_policy {
    int log_enabled;
    int default_decision;
    struct boss_rule *rules;
    int nrules;
};

int  policy_load(struct boss_policy *p, const char *path);
void policy_free(struct boss_policy *p);
int  policy_decide(const struct boss_policy *p, uid_t uid);
int  policy_ensure_file(const char *path);
int  boss_policy_main(int argc, char **argv);

/* ---- pty.c ---- */
int  boss_pty_open(int *master, int *slave, char *name, size_t namelen);
void boss_pty_setup(int master, int rows, int cols);

/* ---- 各入口 ---- */
int  boss_daemon_main(int argc, char **argv);
int  boss_su_main(int argc, char **argv);
int  boss_init_main(int argc, char **argv);

/* ---- 任务3 入口 ---- */
int  boss_boot_main(int argc, char **argv);    /* 开机阶段编排（顺序不能乱） */
int  boss_install(void);                       /* bossinit.c：落盘 + policy（幂等） */
int  boss_resetprop_main(int argc, char **argv);   /* B1 属性改写 */
int  boss_sepolicy_main(int argc, char **argv);    /* C1 sepolicy 工具 */
int  boss_selinux_main(int argc, char **argv);     /* 任务4 SELinux 解决与规则注入 */
/* ---- 任务4 · libsepol 内置后端 ----
 * 引擎链第一级（src/sepol_backend.c）：不 fork、不落临时文件、不依赖设备上
 * 恰好存在 magiskpolicy。早期注入（init selinux_setup）时 /data 还没挂载，
 * 外部引擎那条退路往往根本不存在，所以这一级是路径 A 能否成立的前提。
 *
 * 返回 0 全部应用 / 1 失败 / 2 无引擎 / 3 部分应用；
 * 返回 -1 = 本后端不可用（没 vendor libsepol），调用方应交给下一级。
 * 0 与 3 都表示"这一级已经接手"，不能再往下走，否则规则会被应用两遍。
 */
struct inject_result {
    int applied;   /* 确认应用成功 */
    int skipped;   /* 目标 type/class 不存在等"可容忍"失败 */
    int failed;    /* 真正的错误 */
};
int  sepol_builtin_apply(const char *in, const char *out, const char **rules,
                         int n, int live, struct inject_result *res);

/* 任务4 给 sepolicy 工具用的注入入口：
 * 返回 0 全部应用 / 2 无引擎（规则已进 pending，不是成功）/ 3 部分应用 / 1 失败。
 * in/out 为 NULL 且 live=1 时直接改内核当前策略。 */
int  boss_selinux_inject(const char *in, const char *out, int live,
                         const char **rules, int n);
int  boss_script_main(int argc, char **argv);      /* B3 boot 阶段脚本执行器 */
int  boss_module_main(int argc, char **argv);      /* B2 模块 / overlay 挂载 + D1 */
int  boss_sh_main(int argc, char **argv);          /* A2 standalone shell */
int  boss_applet_main(int argc, char **argv);      /* A1 applet symlink 管理 */

/* ---- 任务6：manager 身份识别（src/manager.c）----
 * 默认策略是 deny，而 App 连 /data/adb/boss 都进不去，没法自己写规则。
 * 所以 manager 的放行由 daemon 侧认定，判定只用内核给的 uid。 */
int  boss_manager_load(uid_t *uid);
int  boss_manager_set(uid_t uid);          /* uid=0 表示清除 */
int  boss_manager_register(uid_t uid, const char *caller);
int  boss_manager_is(uid_t uid, const char *caller);
const char *boss_manager_pkg(void);

/* ---- 任务6：授权弹窗（src/prompt.c）----
 * fork-per-client 的 daemon 里，等待者与答复者是两个进程，
 * 用 BOSS_DIR 下的 .req / .ans 文件配对（详见 prompt.c 顶部注释）。 */
int  prompt_create(uid_t uid, const char *caller, const char *cmd, char *id, size_t idsz);
void prompt_remove(const char *id);
int  prompt_answer(const char *id, int allow);
int  prompt_wait(const char *id, int sock, int timeout_ms, int *decision);
int  prompt_pending(char *out, size_t cap);

/* ---- 任务5 入口 ---- */
int  boss_systemless_main(int argc, char **argv);  /* 无修改系统逻辑：清单应用 + 零写入自检 */
int  boss_hide_main(int argc, char **argv);        /* 特典：隐藏名单 / 挂载痕迹治理 / 属性伪装 */

/* ---- mntinfo.c：/proc/self/mountinfo 解析 ----
 * 任务5 的两件事都建立在同一份事实上：读挂载表。
 *   · systemless verify：证明"改动只来自 /data，没有落 /system"
 *   · hide：找出 BOSS 引入的挂载，好把它们从目标进程视野里摘掉
 * 所以解析只写一份，两边共用——各写一份迟早会对不上。 */
struct boss_mount {
    int  id;
    int  parent;
    char src[512];    /* 挂载源 */
    char tgt[512];    /* 挂载点 */
    char type[64];    /* 文件系统类型 */
    char opts[256];   /* 挂载选项（含 ro/rw） */
};

/* 扫描当前进程所见挂载表，返回条数（<0 出错）。调用方负责 free(*out) 里每个元素与数组。 */
int  boss_mount_scan(struct boss_mount ***out);
/* 这条挂载是不是 BOSS 引入的：源在 BOSS_DIR 下，或是盖在只读分区上的 tmpfs 覆盖层。 */
int  boss_mount_is_boss(const struct boss_mount *m);
/* 只读分区名单（system / vendor / product / system_ext / odm），供 verify 与 hide 共用 */
extern const char *const boss_ro_parts[];

/* ---- applet 分发（A1） ---- */
struct boss_applet {
    const char *name;
    int (*fn)(int argc, char **argv);
    int needs_root;      /* 非 root 直接报错，不静默失败 */
    const char *summary; /* boss --list 用 */
};
int  boss_applet_dispatch(int argc, char **argv);

/* module.c：启用的模块目录（按 id 排序），调用方负责 free 每个元素与数组 */
int  boss_module_dirs(char ***out);
void boss_applet_list(void);

#endif /* BOSS_H */
