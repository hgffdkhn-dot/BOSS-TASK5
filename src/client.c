#define _GNU_SOURCE 1
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "boss.h"

extern char **environ;

static void usage(void)
{
    fprintf(stderr,
        "usage: su [-c <command>] [-s <shell>] [-u <uid>] [-g <gid>]\n"
        "          [-l|--login] [-p|-m|--preserve-environment]\n"
        "          [-Z|--context <domain>] [-M|--mount-master]\n"
        "          [-v|-V|--version] [-h|--help] [LOGIN] [args...]\n");
}

static void send_blob(int sock, const char *buf, size_t len)
{
    if (len && boss_write_full(sock, buf, len) < 0) { /* 对端提前关闭 */ }
}

/* 协议第一步：用 SCM_RIGHTS 把"退出码回传通道"交给 daemon。
 * 所有连接（含探活）都必须先走这一步——否则 daemon 的 recvmsg 会
 * 把请求体的第一个字节当握手吃掉，后面整条流全部错位。 */
int boss_send_handshake(int sock)
{
    int sv[2] = { -1, -1 };
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) return -1;
    if (boss_send_fd(sock, sv[1]) < 0) { close(sv[0]); close(sv[1]); return -1; }
    close(sv[1]);
    return sv[0];
}

/* 安全地把一个 token 追加到命令缓冲区，返回新的 off（保证 <= cap）。
 *
 * 为什么需要单独一个函数：snprintf 返回的是"**本应**写入的长度"，不是实际
 * 写入数。截断时它大于可用空间，于是
 *     off += snprintf(cmd + off, sizeof(cmd) - off, ...)
 * 会让 off 越过 cap；下一轮 sizeof(cmd) - off 是 size_t，无符号下溢成一个
 * 巨大值，那一次的 snprintf 就变成了越界写（栈溢出）。
 * 老应用会传 `su -c <很长的脚本>`，这条路径真的会被走到。
 */
static size_t cmd_append(char *buf, size_t cap, size_t off, const char *s)
{
    if (off >= cap) return off;                   /* 已满 */
    if (off && off + 1 < cap) buf[off++] = ' ';   /* 参数之间补空格 */
    size_t room = cap - off;
    if (room == 0) return off;
    int n = snprintf(buf + off, room, "%s", s);
    if (n < 0) return off;                        /* 编码错误，就地停下 */
    if ((size_t)n >= room) return cap;            /* 发生截断：直接标记满 */
    return off + (size_t)n;
}

/* 常见账户名 → uid。老应用里 `su root -c ...` 这种写法并不少见，
 * 而 Android 的 /etc/passwd 在早期阶段未必可读，查表比解析系统文件稳。 */
static int name_to_uid(const char *s)
{
    if (!strcmp(s, "root"))   return 0;
    if (!strcmp(s, "system")) return 1000;
    if (!strcmp(s, "shell"))  return 2000;
    return -1;
}

/* ------------------------------------------------------------------
 * 向下兼容：协议版本协商
 * ------------------------------------------------------------------
 * 真机上 ramdisk 里的 boss 与 /data/adb/boss/boss 是两份二进制，版本会错开。
 * 两种错开都要能活：
 *   · 新客户端 → 老 daemon（只认 v1）：老 daemon 会回 BOSS_ERR。
 *     我们按 v1 重发一次即可——v1/v2 的结构体布局一致，且本次请求没用到
 *     v2 独有的能力（UI 通道），降级是安全的。
 *   · 老客户端 → 新 daemon：由 daemon 侧放宽版本校验来接住（见 daemon.c）。
 *
 * 只重试"版本不被接受"这一种情况：BOSS_ERR 也可能是别的内部错误，
 * 但重发一次无害（请求是幂等的），换来的是换包后不用手动对齐两份二进制。
 * ------------------------------------------------------------------ */
int boss_negotiate(struct boss_request *req, const char *env_blob,
                   struct boss_response *rep, int *sock_out, int *ver_out)
{
    for (unsigned v = BOSS_PROTO_VER; v >= BOSS_PROTO_MIN; v--) {
        req->version = v;

        int sock = boss_connect();
        if (sock < 0) return -1;

        int status_sock = boss_send_handshake(sock);
        if (boss_write_full(sock, req, sizeof(*req)) < 0) {
            if (status_sock >= 0) close(status_sock);
            close(sock);
            return -1;
        }
        if (req->env_len) send_blob(sock, env_blob, req->env_len);

        if (boss_read_full(sock, rep, sizeof(*rep)) < 0) {
            if (status_sock >= 0) close(status_sock);
            close(sock);
            return -1;
        }

        /* 老 daemon 不接受这个版本：整条请求按下一个版本重来 */
        if (rep->code == BOSS_ERR && v > BOSS_PROTO_MIN) {
            if (status_sock >= 0) close(status_sock);
            close(sock);
            continue;
        }

        *sock_out = sock;
        *ver_out = (int)v;
        return status_sock;
    }
    return -1;
}

int boss_su_main(int argc, char **argv)
{
    struct boss_request req;
    memset(&req, 0, sizeof(req));
    req.magic = BOSS_MAGIC;
    req.version = BOSS_PROTO_VER;
    req.target_uid = 0;
    req.target_gid = 0;

    int i = 1;
    int cmd_mode = 0;
    int uid_seen = 0;      /* 位置参数里的目标用户只认第一个 */
    char cmd[BOSS_MAX_CMD] = { 0 };

    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-V") || !strcmp(a, "-v") || !strcmp(a, "--version")) {
            /* Magisk 的 -v / -V 都是版本；老脚本两种都写，都认 */
            printf("%s\n", BOSS_VERSION);
            return 0;
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage();
            return 0;
        } else if (!strcmp(a, "-c") && i + 1 < argc) {
            /* 向下兼容：`-c` 之后的所有内容拼成一条命令。
             * 老应用常见 `su -c ls -l`（没加引号），Magisk 也是这么处理的——
             * 只取第一个 token 会让这类调用静默执行错命令。 */
            cmd_mode = 1;
            i++;
            size_t off = 0;
            for (; i < argc; i++)
                off = cmd_append(cmd, sizeof(cmd), off, argv[i]);
            break;
        } else if (!strcmp(a, "--command") && i + 1 < argc) {
            cmd_mode = 1;
            i++;
            boss_copy(cmd, sizeof(cmd), argv[i]);
        } else if (!strcmp(a, "-s") && i + 1 < argc) {
            i++;
            boss_copy(req.shell, sizeof(req.shell), argv[i]);
        } else if (!strcmp(a, "-u") && i + 1 < argc) {
            i++;
            req.target_uid = (uint32_t)atoi(argv[i]);
        } else if (!strcmp(a, "-g") && i + 1 < argc) {
            i++;
            req.target_gid = (uint32_t)atoi(argv[i]);
        } else if (!strcmp(a, "-Z") || !strcmp(a, "--context")) {
            /* Magisk 的 -Z；也接受老写法 --context */
            if (i + 1 < argc) { i++; boss_copy(req.context, sizeof(req.context), argv[i]); }
        } else if (!strcmp(a, "-M") || !strcmp(a, "--mount-master")) {
            /* Magisk 语义：在全局 mount namespace 里执行。
             * bossd 由 init 拉起、本身就在全局 namespace，天然满足。
             * 接受它（而不是报错或当未知参数丢掉），老应用才不会卡在这里。 */
            req.flags |= BOSS_F_MOUNT_MASTER;
        } else if (!strcmp(a, "-l") || !strcmp(a, "--login") || !strcmp(a, "-")) {
            req.flags |= BOSS_F_LOGIN;
        } else if (!strcmp(a, "-p") || !strcmp(a, "--preserve-environment")) {
            req.flags |= BOSS_F_KEEPENV;
        } else if (!strcmp(a, "-mm") || !strcmp(a, "-m")) {
            req.flags |= BOSS_F_KEEPENV;
        } else if (a[0] != '-' && !uid_seen) {
            /* 位置参数当目标用户：`su root -c ...`、`su 0 -c ...`（老脚本常见写法） */
            char *end = NULL;
            long n = strtol(a, &end, 10);
            if (end && *end == '\0' && n >= 0) {
                req.target_uid = (uint32_t)n;
                uid_seen = 1;
            } else {
                int u = name_to_uid(a);
                if (u >= 0) { req.target_uid = (uint32_t)u; uid_seen = 1; }
            }
            /* 认不出来就当普通参数忽略：宁可少解析一个，也不要把命令吃掉 */
        }
        /* 其余未知选项一律忽略——老应用传的参数比我们实现的多，
         * 报错会让它们判定"此设备无 root"，静默接受才是对的。 */
    }
    boss_copy(req.command, sizeof(req.command), cmd);
    (void)cmd_mode;

    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        req.rows = ws.ws_row;
        req.cols = ws.ws_col;
    }

    /* 环境变量 blob（daemon 端只在 KEEPENV 时整体使用，否则只取 TERM） */
    size_t env_len = 0;
    for (char **e = environ; *e; e++)
        env_len = boss_blob_add(NULL, env_len, 0, *e);
    char *env_blob = NULL;
    if (env_len) {
        env_blob = calloc(1, env_len + 8);
        size_t off = 0;
        for (char **e = environ; *e && off < env_len; e++)
            off = boss_blob_add(env_blob, off, env_len, *e);
        req.env_len = (uint32_t)off;
    }

    int sock = -1, used_ver = (int)BOSS_PROTO_VER;
    struct boss_response rep;
    memset(&rep, 0, sizeof(rep));

    int status_sock = boss_negotiate(&req, env_blob, &rep, &sock, &used_ver);
    if (status_sock < 0 && sock < 0) {
        fprintf(stderr, "su: bossd 未运行或 BOSS 未安装\n");
        free(env_blob);
        return 1;
    }
    if (sock < 0) {
        fprintf(stderr, "su: 与 bossd 通信失败\n");
        free(env_blob);
        return 1;
    }

    if (rep.code != BOSS_OK) {
        const char *why = rep.code == BOSS_DENIED ? "策略拒绝" :
                          (rep.code == BOSS_PROMPT ? "等待 BOSS 前端授权" : "内部错误");
        fprintf(stderr, "su: %s (uid=%u, proto=v%d)\n", why, (unsigned)getuid(), used_ver);
        close(sock);
        free(env_blob);
        return 1;
    }

    /* 交互式：把自己的终端切成 raw，让 ^C / 补全等直接透传 */
    struct termios saved;
    int tty = isatty(STDIN_FILENO);
    int restored = 0;
    if (tty && tcgetattr(STDIN_FILENO, &saved) == 0) {
        struct termios raw = saved;
        raw.c_lflag &= ~(tcflag_t)(ECHO | ICANON | ISIG | IEXTEN);
        raw.c_iflag &= ~(tcflag_t)(ICRNL | IXON);
        raw.c_oflag &= ~(tcflag_t)(OPOST);
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        restored = 1;
    }

    char buf[8192];
    int exit_code = 0, got_code = 0;
    struct pollfd fds[3];
    int stdin_open = 1;

    while (1) {
        int nfd = 0;
        fds[nfd].fd = sock;      fds[nfd].events = POLLIN; fds[nfd].revents = 0; nfd++;
        fds[nfd].fd = status_sock >= 0 ? status_sock : -1; fds[nfd].events = POLLIN; fds[nfd].revents = 0; nfd++;
        if (stdin_open) { fds[nfd].fd = STDIN_FILENO; fds[nfd].events = POLLIN; fds[nfd].revents = 0; nfd++; }

        int r = poll(fds, (nfds_t)nfd, -1);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t n = read(sock, buf, sizeof(buf));
            if (n <= 0) break;
            (void)boss_write_full(STDOUT_FILENO, buf, (size_t)n);
        }
        if (fds[1].fd >= 0 && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            uint32_t ec = 0;
            ssize_t n = read(fds[1].fd, &ec, sizeof(ec));
            if (n == (ssize_t)sizeof(ec)) { exit_code = (int)ec; got_code = 1; }
            break;
        }
        if (stdin_open && (fds[2].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) {
                /* 注意：这里不能 shutdown(sock, SHUT_WR)。
                 * 一旦把写方向关掉，daemon 会认为客户端消失而杀掉子进程，
                 * 导致 -c 的输出还没吐完就被截断。保持连接即可。 */
                stdin_open = 0;
            } else if (boss_write_full(sock, buf, (size_t)n) < 0) {
                break;
            }
        }
    }

    /* 数据结束到退出码到达有一点时间差，等一下再收 */
    if (!got_code && status_sock >= 0) {
        struct pollfd p = { .fd = status_sock, .events = POLLIN };
        if (poll(&p, 1, 300) > 0) {
            uint32_t ec = 0;
            if (read(status_sock, &ec, sizeof(ec)) == (ssize_t)sizeof(ec)) exit_code = (int)ec;
        }
    }

    if (restored) tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    if (status_sock >= 0) close(status_sock);
    close(sock);
    free(env_blob);
    return exit_code;
}
