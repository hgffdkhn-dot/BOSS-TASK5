/* 任务6 · 授权弹窗（prompt）的真实实现
 *
 * 任务2 把三元语义（allow / deny / prompt）留了下来，但 prompt 一直"按拒绝
 * 处理"——BOSS_PROMPT 只是个返回码，没人等用户。任务6 要把它补成真的：
 * 用户点一下，命令才继续跑。
 *
 * 为什么用**文件**而不是共享内存 / 新 socket：
 *   daemon 是 fork-per-client 的（见 boss_daemon_main）。等待中的 handler 与
 *   答复它的 UI handler 是两个互不相干的进程，共享内存要额外建映射，新 socket
 *   要新的一套连接管理。而 BOSS_DIR 本来就是 root 私有的 0700 目录，
 *   一个 .req + 一个 .ans 文件就够了，代价是轮询（200ms 一轮，只在弹窗期间）。
 *
 * 两个必须做对的细节：
 *   1. **等待期间要同时看着客户端 socket。** 用户没点、app 先退了，请求就该
 *      作废，否则 handler 会白等到超时，还留一个孤儿 .req 文件。
 *   2. **id 必须校验。** id 来自 App（不可信输入），会拼进路径。只接受
 *      [A-Za-z0-9_.-]，否则 "a/../../etc/x" 这类能写到任意位置。
 */
#define _GNU_SOURCE 1
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "boss.h"

#define BOSS_PROMPT_DIR BOSS_DIR "/prompt"
#define PROMPT_POLL_MS  200
#define PROMPT_TIMEOUT_MS 60000   /* 一分钟没人点就当拒绝，别把开机卡住 */

static int id_ok(const char *id)
{
    if (!id || !*id || strlen(id) > 96) return 0;
    for (const char *p = id; *p; p++) {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-')
            continue;
        return 0;   /* 含 '/'、空格、'..' 之外的怪字符一律拒绝 */
    }
    return 1;
}

static void path_of(char *out, size_t n, const char *id, const char *ext)
{
    snprintf(out, n, "%s/%s.%s", BOSS_PROMPT_DIR, id, ext);
}

int prompt_create(uid_t uid, const char *caller, const char *cmd,
                  char *id, size_t idsz)
{
    if (!id || idsz < 32) return -1;
    if (boss_mkdirs(BOSS_PROMPT_DIR, 0700) < 0) return -1;

    snprintf(id, idsz, "%u_%lld_%d", (unsigned)uid,
             (long long)time(NULL), (int)getpid());

    char p[512];
    path_of(p, sizeof(p), id, "req");
    FILE *fp = fopen(p, "we");
    if (!fp) return -1;
    fprintf(fp, "uid=%u\ncaller=%s\ncmd=%s\nts=%lld\n",
            (unsigned)uid, caller ? caller : "?", cmd ? cmd : "",
            (long long)time(NULL));
    fclose(fp);
    chmod(p, 0600);
    return 0;
}

void prompt_remove(const char *id)
{
    if (!id_ok(id)) return;
    char p[512];
    path_of(p, sizeof(p), id, "req");
    unlink(p);
    path_of(p, sizeof(p), id, "ans");
    unlink(p);
}

/* App 答复。allow=1 放行，0 拒绝。 */
int prompt_answer(const char *id, int allow)
{
    if (!id_ok(id)) return -1;
    char rp[512];
    path_of(rp, sizeof(rp), id, "req");
    FILE *f = fopen(rp, "re");
    if (!f) return -1;      /* 请求已经不在了（超时/客户端走了） */
    fclose(f);

    char ap[512];
    path_of(ap, sizeof(ap), id, "ans");
    FILE *fp = fopen(ap, "we");
    if (!fp) return -1;
    fputs(allow ? "allow\n" : "deny\n", fp);
    fclose(fp);
    return 0;
}

/* 等待答复。
 * 返回  0 = 拿到答复（*decision 为 1 放行 / 0 拒绝）
 *       1 = 超时（按拒绝处理）
 *      -1 = 客户端已经消失（按拒绝处理）
 * 出错也一律往"拒绝"走：弹窗这条路上，宁可让用户重按一次，也不能默认放行。 */
int prompt_wait(const char *id, int sock, int timeout_ms, int *decision)
{
    if (!decision) return -1;
    *decision = 0;
    if (!id_ok(id)) return -1;

    int wait_ms = timeout_ms > 0 ? timeout_ms : PROMPT_TIMEOUT_MS;
    int elapsed = 0;
    char ap[512];
    path_of(ap, sizeof(ap), id, "ans");

    while (elapsed < wait_ms) {
        FILE *fp = fopen(ap, "re");
        if (fp) {
            char line[32] = { 0 };
            if (fgets(line, sizeof(line), fp)) *decision = (strncmp(line, "allow", 5) == 0);
            fclose(fp);
            return 0;
        }

        /* 同时看着客户端：它走了就别等了 */
        if (sock >= 0) {
            struct pollfd pfd;
            pfd.fd = sock;
            pfd.events = POLLIN;
            pfd.revents = 0;
            int r = poll(&pfd, 1, PROMPT_POLL_MS);
            if (r > 0 && (pfd.revents & (POLLHUP | POLLERR))) return -1;
            if (r > 0 && (pfd.revents & POLLIN)) {
                /* 客户端在等答复时不该发数据；发了说明它已经不对劲了 */
                char tmp[64];
                ssize_t n = read(sock, tmp, sizeof(tmp));
                if (n <= 0) return -1;
            }
        } else {
            usleep(PROMPT_POLL_MS * 1000);
        }
        elapsed += PROMPT_POLL_MS;
    }
    return 1;
}

/* 列出待决请求，给 App 轮询用。
 * 输出格式（一行一条，App 按行切）：
 *   <id>\t<uid>\t<caller>\t<cmd>
 * 为什么是 TSV 而不是 JSON：这一层没有 JSON 解析器（boss 只有 libc），
 * 而 cmd 里可能有空格，用空格分隔必然解析错。制表符同样可能出现在 cmd 里，
 * 所以 cmd 放最后、整行剩下的都算 cmd。 */
int prompt_pending(char *out, size_t cap)
{
    if (!out || cap == 0) return -1;
    out[0] = '\0';
    size_t off = 0;

    DIR *d = opendir(BOSS_PROMPT_DIR);
    if (!d) return 0;       /* 没有待决请求不是错误 */

    struct dirent *e;
    while ((e = readdir(d))) {
        const char *name = e->d_name;
        size_t nl = strlen(name);
        if (nl < 5 || strcmp(name + nl - 4, ".req") != 0) continue;

        char id[128];
        snprintf(id, sizeof(id), "%s", name);
        id[nl - 4] = '\0';
        if (!id_ok(id)) continue;

        char p[512];
        path_of(p, sizeof(p), id, "req");
        FILE *fp = fopen(p, "re");
        if (!fp) continue;

        char line[256];
        char uid[32] = "?", caller[128] = "?", cmd[256] = "";
        char ts[64] = "";
        while (fgets(line, sizeof(line), fp)) {
            char *nlp = strchr(line, '\n');
            if (nlp) *nlp = '\0';
            if (!strncmp(line, "uid=", 4))         snprintf(uid, sizeof(uid), "%.31s", line + 4);
            else if (!strncmp(line, "caller=", 7)) snprintf(caller, sizeof(caller), "%.127s", line + 7);
            else if (!strncmp(line, "cmd=", 4))    snprintf(cmd, sizeof(cmd), "%.255s", line + 4);
            else if (!strncmp(line, "ts=", 3))     snprintf(ts, sizeof(ts), "%.63s", line + 3);
        }
        fclose(fp);

        int n = snprintf(out + off, cap - off, "%s\t%s\t%s\t%s\t%s\n",
                         id, uid, caller, cmd, ts);
        if (n < 0 || (size_t)n >= cap - off) break;
        off += (size_t)n;
    }
    closedir(d);
    return (int)off;
}
