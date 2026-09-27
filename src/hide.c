/* hide.c — 任务5 B 面：特典逻辑（隐蔽 / 日用）
 *
 * "无修改系统逻辑"解决的是"怎么改"，这一面解决的是"改完之后还能不能被看见"。
 * BOSS 与 Magisk 的定位差异就在这里：Magisk 的能力是公开的，BOSS 主打隐蔽。
 *
 * 三件特典，按"代价从小到大"排：
 *   ① 挂载痕迹治理 —— 先把 BOSS 引入的挂载**列清楚**（mounts）。
 *      看不见自己的暴露面，就谈不上治理；这一步离机就能验。
 *   ② 隐藏名单（denylist）—— 对名单里的进程，把 BOSS 的挂载从它视野里摘掉。
 *   ③ 属性伪装 —— 让设备看起来像一台没被改过的 user 版机器。
 *
 * 关于 ②，必须说清一个取舍（真机上会直接影响效果）：
 *   Android 的 app 进程与 zygote **共享 mount namespace**，所以"给某个 app
 *   单独摘挂载"在无注入的前提下做不到——我们在 A 进程的 ns 里 umount，
 *   同 ns 的 B 进程也一起看不见了。真正按进程隔离要走到 zygisk 式的注入
 *   （在 app 进程内 unshare(CLONE_NEWNS) 后再 umount），那属于后续任务。
 *   这里做的是无注入版本：命中名单就摘，代价是共享 ns 的进程一起生效。
 *   取舍记录在文档里，不在代码里偷偷改语义。
 *
 * 关于 ③ 的边界：属性伪装只改属性区的值，是检测面里的**一层**，不是全部。
 * 它不承诺、也不以保证绕过任何第三方完整性/风控判定为目标。
 */
#define _GNU_SOURCE 1
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "boss.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MAX_DENY 256
#define MAX_NAME 192

/* ------------------------------------------------------------------ */
/* 隐藏名单                                                            */
/* ------------------------------------------------------------------ */

static int deny_load(char list[MAX_DENY][MAX_NAME])
{
    FILE *fp = fopen(BOSS_DENY_CONF, "re");
    if (!fp) return 0;
    int n = 0;
    char line[MAX_NAME + 32];
    while (fgets(line, sizeof(line), fp) && n < MAX_DENY) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        char *e = s + strlen(s);
        while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
            *--e = '\0';
        if (!*s || *s == '#') continue;
        boss_copy(list[n], MAX_NAME, s);
        n++;
    }
    fclose(fp);
    return n;
}

static int deny_save(char list[MAX_DENY][MAX_NAME], int n)
{
    if (boss_mkdirs(BOSS_DIR, 0700) < 0) return -1;
    FILE *fp = fopen(BOSS_DENY_CONF, "we");
    if (!fp) return -1;
    fprintf(fp, "# BOSS 隐藏名单：每行一个包名或进程名（# 开头是注释）\n");
    for (int i = 0; i < n; i++) fprintf(fp, "%s\n", list[i]);
    fclose(fp);
    return 0;
}

static int cmd_denylist(int argc, char **argv)
{
    static char list[MAX_DENY][MAX_NAME];
    int n = deny_load(list);

    if (argc < 1) {
        printf("隐藏名单（%d 项）：\n", n);
        for (int i = 0; i < n; i++) printf("  %s\n", list[i]);
        return 0;
    }
    if (!strcmp(argv[0], "list")) {
        for (int i = 0; i < n; i++) printf("%s\n", list[i]);
        return 0;
    }
    if (argc < 2) { fprintf(stderr, "用法: boss hide denylist add|del <name>\n"); return 1; }

    const char *name = argv[1];
    if (!strcmp(argv[0], "add")) {
        for (int i = 0; i < n; i++)
            if (!strcmp(list[i], name)) return 0;      /* 幂等 */
        if (n >= MAX_DENY) { fprintf(stderr, "名单已满（%d）\n", MAX_DENY); return 1; }
        boss_copy(list[n], MAX_NAME, name);
        n++;
        return deny_save(list, n) < 0 ? 1 : 0;
    }
    if (!strcmp(argv[0], "del") || !strcmp(argv[0], "rm")) {
        int w = 0;
        for (int i = 0; i < n; i++) {
            if (!strcmp(list[i], name)) continue;
            /* w == i 时是自拷贝：boss_copy 内部是 snprintf(dst, n, "%s", src)，
             * 源和目的同一块缓冲区属于未定义行为，glibc 上会直接把它清成空串
             * ——症状是"删掉一项，结果整个名单被清空"。 */
            if (w != i) boss_copy(list[w], MAX_NAME, list[i]);
            w++;
        }
        return deny_save(list, w) < 0 ? 1 : 0;
    }
    fprintf(stderr, "用法: boss hide denylist add|del <name>\n");
    return 1;
}

/* ------------------------------------------------------------------ */
/* 进程扫描                                                            */
/* ------------------------------------------------------------------ */

/* 进程启动时间（/proc/PID/stat 第 22 个字段）。
 * 为什么需要它：pid 会复用。只记 pid 的话，一个短命进程退出后 pid 被新进程
 * 拿走，我们就会以为"已经处理过"而漏摘——正好是隐藏功能最怕的漏网。 */
static int proc_starttime(pid_t pid, unsigned long long *out)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *fp = fopen(path, "re");
    if (!fp) return -1;
    /* comm 字段可能自带空格和括号（"bash"、"kworker/0:1"），
     * 所以先一路跳到右括号，再从 state 字段开始按 token 数。 */
    int ch;
    while ((ch = fgetc(fp)) != EOF && ch != ')') ;
    unsigned long long st = 0;
    char buf[512];
    if (!fgets(buf, sizeof(buf), fp)) { fclose(fp); return -1; }
    fclose(fp);

    /* state(3) ppid(4) pgrp session tty_nr tpgid flags minflt cminflt majflt
     * cmajflt utime stime cutime cstime priority nice num_threads itrealvalue
     * starttime(22) —— 从 state 数起，starttime 是第 20 个 token */
    char *p = buf;
    for (int k = 0; k < 19; k++) {
        while (*p == ' ') p++;
        while (*p && *p != ' ') p++;
        if (!*p) return -1;
    }
    while (*p == ' ') p++;
    st = strtoull(p, NULL, 10);
    if (out) *out = st;
    return 0;
}

static int proc_cmdline(pid_t pid, char *out, size_t n)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t r = read(fd, out, n - 1);
    close(fd);
    if (r <= 0) return -1;
    out[r] = '\0';
    /* cmdline 以 NUL 分隔参数：只取第一段（Android app 的第一段就是包名） */
    for (size_t i = 0; i < (size_t)r; i++)
        if (out[i] == '\0') { out[i] = '\0'; break; }
    return 0;
}

/* 已处理记录：pid + starttime */
static int state_has(pid_t pid, unsigned long long st)
{
    FILE *fp = fopen(BOSS_DENY_STATE, "re");
    if (!fp) return 0;
    char line[128];
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        unsigned long long p = 0, s = 0;
        if (sscanf(line, "%llu %llu", &p, &s) == 2 &&
            p == (unsigned long long)pid && s == st) { found = 1; break; }
    }
    fclose(fp);
    return found;
}

static void state_add(pid_t pid, unsigned long long st)
{
    if (boss_mkdirs(BOSS_DIR, 0700) < 0) return;
    FILE *fp = fopen(BOSS_DENY_STATE, "ae");
    if (!fp) return;
    fprintf(fp, "%d %llu\n", pid, st);
    fclose(fp);
}

/* ------------------------------------------------------------------ */
/* 摘挂载：在目标进程的 mount namespace 里执行                          */
/* ------------------------------------------------------------------ */

/* 在 pid 的 mount ns 里把 BOSS 引入的挂载摘掉。
 *
 * 为什么必须 fork：setns 会**改变本进程**的命名空间归属，直接在本进程里做
 * 等于把自己也挪过去，之后再也回不到原来的挂载视图（init 阶段尤其致命）。
 * fork 之后子进程随便折腾，做完就 _exit，父进程不受影响。
 *
 * 为什么 umount2 + MNT_DETACH（lazy）：目标挂载可能正被某进程占用，
 * 普通 umount 会 EBUSY；lazy 卸载立刻让它从路径空间消失（检测看不到），
 * 真正的回收等引用归零。我们要的是"看不见"，不是"立刻释放"。
 */
static int umount_in_ns(pid_t pid, int dry)
{
    char nspath[64];
    snprintf(nspath, sizeof(nspath), "/proc/%d/ns/mnt", pid);

    pid_t p = fork();
    if (p < 0) return -1;
    if (p == 0) {
        int fd = open(nspath, O_RDONLY | O_CLOEXEC);
        if (fd < 0) _exit(3);                 /* 目标进程已退出 / 没权限 */
        if (setns(fd, CLONE_NEWNS) != 0) _exit(3);
        close(fd);

        /* 换 ns 之后**重新**扫挂载表：目标 ns 的挂载视图与我们自己的不同，
         * 拿宿主的列表去 umount 会大面积 EINVAL（"没挂这个东西"）。 */
        struct boss_mount **ms = NULL;
        int nm = boss_mount_scan(&ms);
        if (nm <= 0) _exit(4);

        int done = 0, fail = 0;
        for (int i = nm - 1; i >= 0; i--) {   /* 逆序：子挂载先摘 */
            if (!boss_mount_is_boss(ms[i])) continue;
            if (dry) { printf("    would umount %s\n", ms[i]->tgt); done++; continue; }
            if (umount2(ms[i]->tgt, MNT_DETACH) == 0) done++;
            else fail++;
        }
        for (int i = 0; i < nm; i++) free(ms[i]);
        free(ms);
        _exit(done ? 0 : (fail ? 5 : 0));
    }

    int st = 0;
    while (waitpid(p, &st, 0) < 0 && errno == EINTR) ;
    if (!WIFEXITED(st)) return -1;
    return WEXITSTATUS(st);
}

static int cmd_umount(int argc, char **argv)
{
    if (argc < 1) { fprintf(stderr, "用法: boss hide umount <pid> [--dry]\n"); return 1; }
    pid_t pid = atoi(argv[0]);
    if (pid <= 0) { fprintf(stderr, "pid 无效\n"); return 1; }

    int dry = 0;
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--dry")) dry = 1;

    char cmd[192] = { 0 };
    proc_cmdline(pid, cmd, sizeof(cmd));
    printf("hide: 在 pid %d (%s) 的挂载命名空间里摘除 BOSS 挂载%s\n",
           pid, cmd[0] ? cmd : "?", dry ? "（dry run）" : "");

    int rc = umount_in_ns(pid, dry);
    if (rc == 0) { printf("  完成\n"); return 0; }
    /* 返回码要说人话：3 是"进不去"（最常见，非 root 或进程已死），
     * 沙盒与非 root 环境下必然是这个，不要报成"摘挂载失败"误导排查方向。 */
    if (rc == 3) fprintf(stderr, "  无法进入目标挂载命名空间（非 root / 进程已退出 / 内核不支持 ns）\n");
    else if (rc == 5) fprintf(stderr, "  挂载存在但未能摘除（SELinux 或占用）\n");
    else fprintf(stderr, "  失败 rc=%d\n", rc);
    return 1;
}

/* ------------------------------------------------------------------ */
/* 扫描与守护                                                          */
/* ------------------------------------------------------------------ */

static int scan_once(int dry)
{
    static char list[MAX_DENY][MAX_NAME];
    int n = deny_load(list);
    if (n == 0) return 0;

    DIR *d = opendir("/proc");
    if (!d) return -1;

    int hit = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        pid_t pid = atoi(e->d_name);
        char cmd[MAX_NAME];
        if (proc_cmdline(pid, cmd, sizeof(cmd)) != 0) continue;

        int match = 0;
        for (int i = 0; i < n; i++) {
            /* 包名完全一致，或进程名是包名（app 的常见形态） */
            if (!strcmp(list[i], cmd) || !strncmp(cmd, list[i], strlen(list[i]))) {
                match = 1; break;
            }
        }
        if (!match) continue;

        unsigned long long st = 0;
        if (proc_starttime(pid, &st) != 0) continue;
        if (state_has(pid, st)) continue;     /* 处理过就不再重复，避免空转 */

        printf("hide: 命中 %s（pid %d）\n", cmd, pid);
        boss_log_line("hide: %s pid=%d", cmd, pid);
        int rc = umount_in_ns(pid, dry);
        /* 非 root 下必然失败：记下来但不退出——守护进程退出比失败更糟，
         * 后面所有进程都不会再被处理（任务3 坑 7 的同一个教训）。 */
        if (rc != 0 && !dry)
            boss_log_line("hide: umount pid=%d rc=%d（非 root 下为常态）", pid, rc);
        state_add(pid, st);
        hit++;
    }
    closedir(d);
    return hit;
}

static int cmd_daemon(int argc, char **argv)
{
    int once = 0;
    int interval_ms = 500;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--once")) once = 1;
        else if (!strcmp(argv[i], "--interval") && i + 1 < argc) interval_ms = atoi(argv[++i]);
    }
    if (interval_ms < 50) interval_ms = 50;

    /* 自改名：ps / top 里出现的进程名本身就是一个检测面。
     * 只对**自己**生效，不影响任何其它进程。 */
    prctl(PR_SET_NAME, (unsigned long)"boss-hide", 0, 0, 0);

    boss_log_line("hide: daemon start once=%d interval=%dms", once, interval_ms);
    for (;;) {
        scan_once(0);
        if (once) break;
        struct timespec ts;
        ts.tv_sec = interval_ms / 1000;
        ts.tv_nsec = (long)(interval_ms % 1000) * 1000000L;
        nanosleep(&ts, NULL);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 属性伪装（特典 ③）                                                  */
/* ------------------------------------------------------------------ */

static const char *const props_template[] = {
    "# BOSS 属性伪装清单（systemless：只改属性区的值，不碰 build.prop 文件）",
    "#",
    "# 边界说明（务必读完再改）：",
    "#   · 这是检测面里的**一层**，不是全部。它不承诺、也不以保证绕过任何",
    "#     第三方完整性/风控判定为目标；BOSS 不做欺骗用户的隐藏。",
    "#   · 动态属性（sys.usb.state 等）会被系统服务随时改回，写了也会漂。",
    "#   · 每条都可以用 `boss resetprop -d <name>` 原样撤回。",
    "#",
    "# 让设备呈现为未解锁的 user 版：",
    "prop ro.debuggable 0",
    "prop ro.secure 1",
    "prop ro.build.type user",
    "prop ro.boot.vbmeta.device_state locked",
    "prop ro.boot.verifiedbootstate green",
    "prop ro.boot.flash.locked 1",
    "prop ro.boot.veritymode enforcing",
    "prop sys.oem_unlock_allowed 0",
    NULL
};

static int cmd_props(int argc, char **argv)
{
    int save = 0;
    for (int i = 0; i < argc; i++) if (!strcmp(argv[i], "--save-template")) save = 1;

    if (save || access(BOSS_PROPS_CONF, F_OK) != 0) {
        if (boss_mkdirs(BOSS_DIR, 0700) < 0) return 1;
        FILE *fp = fopen(BOSS_PROPS_CONF, "we");
        if (!fp) { fprintf(stderr, "hide: 无法写 %s\n", BOSS_PROPS_CONF); return 1; }
        for (int i = 0; props_template[i]; i++) fprintf(fp, "%s\n", props_template[i]);
        fclose(fp);
        printf("已写入模板 %s\n", BOSS_PROPS_CONF);
        if (save) return 0;
    }

    /* 复用 systemless 的清单引擎：语法、阶段、返回码全部一致，
     * 两边各写一套应用逻辑迟早会在时序上分叉（比如一边忘了 -n）。 */
    const char *bin = access(BOSS_BIN_PATH, X_OK) == 0 ? BOSS_BIN_PATH : NULL;
    char self[512] = { 0 };
    if (!bin) {
        ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (n > 0) { self[n] = '\0'; bin = self; }
    }
    if (!bin) return 2;

    char *a[] = { (char *)bin, (char *)"systemless", (char *)"apply",
                  (char *)"--file", (char *)BOSS_PROPS_CONF, NULL };
    pid_t p = fork();
    if (p == 0) { execv(bin, a); _exit(127); }
    int st = 0;
    while (waitpid(p, &st, 0) < 0 && errno == EINTR) ;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

/* ------------------------------------------------------------------ */

/* --write：把同一份清单落到 BOSS_MOUNT_LOG。
 * 为什么要在开机时留一份：挂载表是"当前视野"，进程一多、ns 一隔离就看不全；
 * 而排查"某模块到底挂了什么"时，最早那一份快照才是有用的。 */
static int cmd_mounts(int argc, char **argv)
{
    int to_log = 0;
    for (int i = 0; i < argc; i++) if (!strcmp(argv[i], "--write")) to_log = 1;

    struct boss_mount **ms = NULL;
    int nm = boss_mount_scan(&ms);
    if (nm < 0) { fprintf(stderr, "hide: 读不到挂载表\n"); return 1; }

    FILE *log = NULL;
    if (to_log && boss_mkdirs(BOSS_DIR, 0700) == 0) log = fopen(BOSS_MOUNT_LOG, "we");

    int shown = 0;
    printf("BOSS 引入的挂载（这些就是暴露面）：\n");
    for (int i = 0; i < nm; i++) {
        if (!boss_mount_is_boss(ms[i])) continue;
        printf("  %-6s %-46s -> %s\n", ms[i]->type, ms[i]->src, ms[i]->tgt);
        if (log) fprintf(log, "%s %s %s\n", ms[i]->type, ms[i]->src, ms[i]->tgt);
        shown++;
    }
    if (!shown) printf("  （无）\n");
    printf("\n共 %d 条（挂载表总条数 %d）。摘除：`boss hide umount <pid>`\n", shown, nm);
    if (log) { fclose(log); printf("已记录到 %s\n", BOSS_MOUNT_LOG); }

    for (int i = 0; i < nm; i++) free(ms[i]);
    free(ms);
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
        "用法: boss hide <命令> [选项]\n"
        "  denylist list|add NAME|del NAME   隐藏名单（" BOSS_DENY_CONF "）\n"
        "  mounts                            列出 BOSS 引入的挂载（暴露面）\n"
        "  umount <pid> [--dry]              在目标进程挂载命名空间里摘除\n"
        "  scan                              扫一遍 /proc，只报告不动作\n"
        "  daemon [--once] [--interval MS]   常驻扫描处理（默认 500ms 一轮）\n"
        "  props [--save-template]           属性伪装清单（systemless 语法）\n\n"
        "取舍：Android app 与 zygote 共享 mount namespace，无注入时无法做到\n"
        "按进程隔离——命中名单即摘，共享同一 ns 的进程会一起生效。\n"
        "真按进程隔离需要 zygisk 式注入，属后续任务。\n");
}

int boss_hide_main(int argc, char **argv)
{
    if (argc >= 1 && argv[0] && !strcmp(argv[0], "hide")) { argc--; argv++; }
    if (argc < 1) { usage(); return 1; }

    if (!strcmp(argv[0], "denylist") || !strcmp(argv[0], "deny"))
        return cmd_denylist(argc - 1, argv + 1);
    if (!strcmp(argv[0], "mounts") || !strcmp(argv[0], "ls"))
        return cmd_mounts(argc - 1, argv + 1);
    if (!strcmp(argv[0], "umount") || !strcmp(argv[0], "hide-pid"))
        return cmd_umount(argc - 1, argv + 1);
    if (!strcmp(argv[0], "scan"))
        return scan_once(1) < 0 ? 1 : 0;
    if (!strcmp(argv[0], "daemon") || !strcmp(argv[0], "watch"))
        return cmd_daemon(argc - 1, argv + 1);
    if (!strcmp(argv[0], "props") || !strcmp(argv[0], "spoof"))
        return cmd_props(argc - 1, argv + 1);

    usage();
    return 1;
}
