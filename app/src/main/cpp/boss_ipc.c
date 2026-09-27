/* BOSS · App 侧 IPC 客户端（任务6）
 *
 * 这段代码刻意"薄"：它只负责把 App 的请求原样送进 bossd，并把子进程输出
 * 原样带回来。任何授权、提权、SELinux 的判断都不在这里——那是 daemon 的活
 * （架构红线第 1 条：daemon 是唯一的特权实体，客户端永远无特权）。
 *
 * 三个不做会出事的细节，都是前辈踩过的：
 *
 *   1. 握手必须第一步（坑 4.1）。不送 fd，daemon 的 recvmsg 会把请求体首字节
 *      当握手吃掉，整条流错位：ping 永远超时，其它命令时好时坏。
 *   2. stdin 结束不要 shutdown(sock, SHUT_WR)（坑 4.2）。App 没有 stdin 可转，
 *      这一点天然满足；但同理，**App 也不能在读完输出前把 socket 关掉**——
 *      daemon 见客户端消失就会杀掉子进程，剩下的输出没了。
 *   3. 必须有整体超时。App 跑在 UI 线程之外，但没有超时的连接会把
 *      "某个命令卡住"变成"App 永远转圈"，而且 exit code 也永远等不到。
 */
#define _GNU_SOURCE 1

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "boss_ipc.h"

#define BOSS_SOCK_NAME "bossd"

#ifndef BOSS_IPC_DEFAULT_TIMEOUT_MS
#define BOSS_IPC_DEFAULT_TIMEOUT_MS 15000
#endif
#ifndef BOSS_IPC_DEFAULT_MAX_OUT
#define BOSS_IPC_DEFAULT_MAX_OUT (1024u * 1024u)
#endif

/* ---------- 底层小工具 ---------- */

static int now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* 抽象命名空间：sun_path[0] = '\0'，名字跟在后面。
 * 文件系统里看不到任何节点——这是 BOSS 低痕跴目标的一部分。
 * 注意长度：不是 sizeof(sockaddr_un)，是 offsetof + 1 + 名字长度。 */
static int boss_connect(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t nlen = strlen(BOSS_SOCK_NAME);
    memcpy(addr.sun_path + 1, BOSS_SOCK_NAME, nlen);
    socklen_t alen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + nlen);

    if (connect(fd, (struct sockaddr *)&addr, alen) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int send_fd(int sock, int fd)
{
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));

    char dummy = 'F';
    struct iovec io;
    io.iov_base = &dummy;
    io.iov_len = 1;
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;

    char cbuf[CMSG_SPACE(sizeof(int))];
    memset(cbuf, 0, sizeof(cbuf));
    msg.msg_control = cbuf;
    msg.msg_controllen = CMSG_SPACE(sizeof(int));

    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof(int));

    return sendmsg(sock, &msg, 0) < 0 ? -1 : 0;
}

static int write_full(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int read_full(int fd, void *buf, size_t len)
{
    char *p = (char *)buf;
    while (len) {
        ssize_t n = read(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;   /* 对端关闭 */
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* 建连第一步：把"退出码回传通道"交给 daemon，返回本地端 fd。
 * 所有连接（含探活）都必须先走这一步。 */
static int send_handshake(int sock)
{
    int sv[2] = { -1, -1 };
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) return -1;
    if (send_fd(sock, sv[1]) < 0) {
        close(sv[0]);
        close(sv[1]);
        return -1;
    }
    close(sv[1]);
    return sv[0];
}

/* ---------- 输出归一 ---------- */

void boss_ipc_normalize(char *buf, size_t *len)
{
    if (!buf || !len) return;
    size_t w = 0;
    for (size_t r = 0; r < *len; r++) {
        if (buf[r] == '\r') {
            /* CRLF → LF；裸 CR 也按换行处理（pty 的常见产物） */
            if (r + 1 < *len && buf[r + 1] == '\n') continue;
            buf[w++] = '\n';
        } else {
            buf[w++] = buf[r];
        }
    }
    *len = w;
    /* 去掉尾部空行：App 按行解析时，尾部空行会变成一个"假条目" */
    while (w > 0 && (buf[w - 1] == '\n' || buf[w - 1] == ' ')) w--;
    *len = w;
    buf[w] = '\0';
}

/* ---------- 对外 API ---------- */

int boss_ipc_ping(void)
{
    int sock = boss_connect();
    if (sock < 0) return 1;

    int status = send_handshake(sock);

    struct boss_ipc_request req;
    memset(&req, 0, sizeof(req));
    req.magic = BOSS_IPC_MAGIC;
    req.version = BOSS_IPC_PROTO_VER;
    req.flags = BOSS_IPC_F_PING | BOSS_IPC_F_NOLOG;

    struct boss_ipc_response rep;
    memset(&rep, 0, sizeof(rep));

    int rc = 0;
    if (write_full(sock, &req, sizeof(req)) < 0) rc = -1;
    else if (read_full(sock, &rep, sizeof(rep)) < 0) rc = -1;
    else rc = (rep.code == BOSS_IPC_OK) ? 0 : -1;

    if (status >= 0) close(status);
    close(sock);
    return rc;
}

int boss_ipc_run(const struct boss_ipc_opts *opts, struct boss_ipc_result *res)
{
    if (!res) return -1;
    memset(res, 0, sizeof(*res));
    res->exit_code = -1;

    int sock = boss_connect();
    if (sock < 0) return -1;

    int status_sock = send_handshake(sock);

    struct boss_ipc_request req;
    memset(&req, 0, sizeof(req));
    req.magic = BOSS_IPC_MAGIC;
    req.version = BOSS_IPC_PROTO_VER;
    req.target_uid = opts && opts->uid ? opts->uid : 0;
    req.target_gid = opts && opts->gid ? opts->gid : 0;
    req.flags = opts ? opts->flags : 0u;
    if (opts && opts->command) {
        /* 截断而不是溢出：struct 是定长的，越界写会踩到 context 字段 */
        snprintf(req.command, sizeof(req.command), "%s", opts->command);
    }
    if (opts && opts->shell)   snprintf(req.shell, sizeof(req.shell), "%s", opts->shell);
    if (opts && opts->context) snprintf(req.context, sizeof(req.context), "%s", opts->context);

    struct boss_ipc_response rep;
    memset(&rep, 0, sizeof(rep));

    if (write_full(sock, &req, sizeof(req)) < 0 ||
        read_full(sock, &rep, sizeof(rep)) < 0) {
        if (status_sock >= 0) close(status_sock);
        close(sock);
        return -1;
    }

    res->code = rep.code;
    if (rep.code != BOSS_IPC_OK) {
        /* DENIED / PROMPT / ERR：没有子进程，也没有输出。
         * 这不是函数失败——调用方要按 res->code 区分语义。 */
        if (status_sock >= 0) close(status_sock);
        close(sock);
        return 0;
    }

    if (req.flags & BOSS_IPC_F_PING) {
        if (status_sock >= 0) close(status_sock);
        close(sock);
        res->exit_code = 0;
        return 0;
    }

    int timeout_ms = (opts && opts->timeout_ms > 0) ? opts->timeout_ms
                                                    : BOSS_IPC_DEFAULT_TIMEOUT_MS;
    size_t max_out = (opts && opts->max_out) ? opts->max_out : BOSS_IPC_DEFAULT_MAX_OUT;

    size_t cap = 8192, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        if (status_sock >= 0) close(status_sock);
        close(sock);
        return -1;
    }

    int deadline = now_ms() + timeout_ms;
    int got_exit = 0;

    while (!got_exit) {
        int remain = deadline - now_ms();
        if (remain <= 0) { res->timed_out = 1; break; }

        struct pollfd fds[2];
        fds[0].fd = sock;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        fds[1].fd = status_sock >= 0 ? status_sock : -1;
        fds[1].events = POLLIN;
        fds[1].revents = 0;

        int r = poll(fds, 2, remain);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) { res->timed_out = 1; break; }

        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            char tmp[8192];
            ssize_t n = read(sock, tmp, sizeof(tmp));
            if (n <= 0) {
                /* 数据通道结束。退出码可能还没到，下面再等一小会儿。 */
                break;
            }
            if (len + (size_t)n + 1 > cap && cap < max_out) {
                size_t ncap = cap * 2;
                if (ncap > max_out) ncap = max_out;
                char *nb = (char *)realloc(buf, ncap);
                if (!nb) { res->timed_out = 1; break; }
                buf = nb;
                cap = ncap;
            }
            size_t room = (len + (size_t)n + 1 <= cap) ? (size_t)n : (cap - len - 1);
            if (room > 0) {
                memcpy(buf + len, tmp, room);
                len += room;
            }
        }

        if (fds[1].fd >= 0 && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            uint32_t ec = 0;
            ssize_t n = read(fds[1].fd, &ec, sizeof(ec));
            if (n == (ssize_t)sizeof(ec)) {
                res->exit_code = (int)ec;
                got_exit = 1;
            }
        }
    }

    /* 数据结束到退出码到达有一点时间差（daemon 要先 waitpid 再写），
     * 等一下再收一次，否则会把 0 当成"没拿到退出码"。 */
    if (!got_exit && status_sock >= 0 && !res->timed_out) {
        struct pollfd p;
        p.fd = status_sock;
        p.events = POLLIN;
        p.revents = 0;
        if (poll(&p, 1, 300) > 0) {
            uint32_t ec = 0;
            if (read(status_sock, &ec, sizeof(ec)) == (ssize_t)sizeof(ec)) {
                res->exit_code = (int)ec;
            }
        }
    }

    buf[len] = '\0';
    boss_ipc_normalize(buf, &len);
    res->out = buf;
    res->out_len = len;

    if (status_sock >= 0) close(status_sock);
    close(sock);
    return 0;
}

void boss_ipc_result_free(struct boss_ipc_result *res)
{
    if (!res) return;
    free(res->out);
    res->out = NULL;
    res->out_len = 0;
}
