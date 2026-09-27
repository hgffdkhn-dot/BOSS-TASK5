#define _GNU_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "boss.h"

/* ------------------------------------------------------------------
 * bossinit：init 阶段的助手（v0.1 骨架 + v0.2 的落点）
 *
 * 为什么需要它：Android 10+ 的两段式 init（2SI）会在第一阶段结束时
 * SwitchRoot——把 / 下的挂载递归移动到 /system 并 chroot 进去，
 * ramdisk 从此消失。也就是说：
 *   · 放在 ramdisk 里的 rc / 二进制，在第二阶段 init 看来不存在；
 *   · 第二阶段 init 只解析 /system/etc/init/hw/init.rc（AOSP LoadBootScripts），
 *     除非用 androidboot.init_rc 整体改写。
 * 因此"su 能不能日用"，取决于能不能在切根前后重新拿到执行权。
 *
 * v0.1：提供 install / start / post-fs-data，覆盖 ramdisk 不被丢弃的布局
 *       （非 2SI、vendor_boot 布局，以及已经由 BOSS App 触发的场景）。
 * v0.2：stage2 + hijack-prep 完成 SwitchRoot 劫持（见 docs 中的方案说明），
 *       做到 /system 零落盘修改。
 * ------------------------------------------------------------------ */

static int copy_self_to(const char *dst)
{
    char self[256] = { 0 };
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n < 0) return -1;

    int in = open(self, O_RDONLY | O_CLOEXEC);
    if (in < 0) return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
    if (out < 0) { close(in); return -1; }

    char buf[65536];
    ssize_t r;
    while ((r = read(in, buf, sizeof(buf))) > 0) {
        if (boss_write_full(out, buf, (size_t)r) < 0) { close(in); close(out); return -1; }
    }
    close(in);
    close(out);
    chmod(dst, 0755);
    return 0;
}

/* hexpatch 的目标路径：必须与 "/system/bin/init" 等长（16 字节，含结尾 NUL），
 * 否则原地替换会覆盖后面的字节——改出一个坏掉的 init 比不改成更糟。
 * "/data/bossinit" 是 14 个字符，剩下两字节补 NUL，正好对齐。 */
#define BOSS_HEXPATCH_PATH "/data/bossinit"
#define INIT_PATH_ORIG     "/system/bin/init"

/* 原地改写 /init 里第二阶段 init 的路径。
 * 这是 Magisk 在**离线 patch boot 镜像**时用的手段；运行时做通常失败
 * （/init 正在执行 → open(O_RDWR) 直接 ETXTBSY）。留着它只为了覆盖那些
 * rootfs 确实可写的老设备，失败是常态，不构成错误。 */
static int hexpatch_init(void)
{
    int fd = open("/init", O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;

    off_t sz = lseek(fd, 0, SEEK_END);
    if (sz <= 0 || sz > (64 * 1024 * 1024)) { close(fd); return -1; }
    lseek(fd, 0, SEEK_SET);

    char *buf = malloc((size_t)sz);
    if (!buf) { close(fd); return -1; }
    ssize_t r = read(fd, buf, (size_t)sz);
    if (r != (ssize_t)sz) { free(buf); close(fd); return -1; }

    size_t orig_len = strlen(INIT_PATH_ORIG);
    char *hit = memmem(buf, (size_t)sz, INIT_PATH_ORIG, orig_len);
    if (!hit) { free(buf); close(fd); return -1; }

    size_t new_len = strlen(BOSS_HEXPATCH_PATH);
    if (new_len > orig_len) { free(buf); close(fd); return -1; }   /* 编译期就该对上 */
    memcpy(hit, BOSS_HEXPATCH_PATH, new_len);
    memset(hit + new_len, 0, orig_len - new_len);

    lseek(fd, 0, SEEK_SET);
    int rc = boss_write_full(fd, buf, (size_t)sz) < 0 ? -1 : 0;
    free(buf);
    close(fd);
    return rc;
}

/* 把 BOSS 落到持久分区：ramdisk 会消失，/data 不会。
 * 幂等，boot 流程与 init 流程都会调它。 */
int boss_install(void)
{
    if (boss_mkdirs(BOSS_DIR, 0700) < 0) return -1;
    chmod(BOSS_DIR, 0700);
    policy_ensure_file(BOSS_POLICY_PATH);
    if (access(BOSS_BIN_PATH, X_OK) != 0 && copy_self_to(BOSS_BIN_PATH) < 0) return -1;
    return 0;
}

static int daemon_alive(void)
{
    int fd = boss_connect();
    if (fd < 0) return 0;
    close(fd);
    return 1;
}

static int start_daemon(void)
{
    const char *bin = access(BOSS_BIN_PATH, X_OK) == 0 ? BOSS_BIN_PATH : NULL;
    if (!bin) return -1;

    pid_t p = fork();
    if (p < 0) return -1;
    if (p == 0) {
        execl(bin, bin, "daemon", (char *)NULL);
        _exit(127);
    }
    for (int i = 0; i < 20; i++) {
        usleep(50 * 1000);
        if (daemon_alive()) return 0;
    }
    return daemon_alive() ? 0 : -1;
}

static int cmd_start(void)
{
    if (boss_install() < 0) return 1;
    if (daemon_alive()) return 0;
    return start_daemon() < 0 ? 1 : 0;
}

/* ------------------------------------------------------------------
 * 早期阶段怎么留日志
 * ------------------------------------------------------------------
 * init 在 selinux_setup 之前就把 stdio 指向了 /dev/null，printf/fprintf
 * 在这个阶段**一个字节都看不到**。/dev/kmsg 是唯一还能写的地方，
 * 真机上 `dmesg | grep boss` 就是这条链路唯一的线索。
 * 开机排障时"没有任何输出"比"报错"难查得多，所以这里必须留。 */
static void kmsg_log(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (fd < 0) return;
    ssize_t w = write(fd, buf, strlen(buf));
    (void)w;   /* 写不进去就算了，别拿日志失败去影响开机 */
    close(fd);
}

/* 早期注入的总开关（读 kernel cmdline）。
 *
 * 为什么用 cmdline 而不是环境变量：selinux_setup 阶段我们对进程的
 * 环境没有控制权（init 传下来的环境几乎是空的），env 在这里不可靠。
 * /proc/cmdline 是 veritpath 的 cmdline_append 能写的，且早期一定可读。
 *
 * 这条退路的价值：早期注入一旦在某机型上导致开不了机，不用重新刷包
 * 就能退回原厂路径（加 boss_selinux=0 即可），否则每次试错都要进
 * recovery 刷回原厂镜像——自救路径的成本直接影响能不能迭代下去。 */
static int cmdline_says_off(const char *key)
{
    char buf[4096] = { 0 };
    int fd = open("/proc/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;

    for (char *p = buf; *p;) {
        while (*p == ' ' || *p == '\t') p++;
        char *tok = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        if (*p) *p++ = '\0';
        if (!strcmp(tok, key)) return 1;
    }
    return 0;
}

/* 真实 init 的位置。
 * BOSS_INIT_REAL 只给离机验证用（tools/stage2_test.sh）：真机上 init
 * 传下来的环境是空的，这个变量不可能被意外带上，等于 inert。
 * 有了它，这条"上机就是砖、沙盒又验不到"的链路才有一个可回归的测试。 */
static const char *real_init_path(void)
{
    const char *ov = getenv("BOSS_INIT_REAL");
    if (ov && access(ov, X_OK) == 0) return ov;
    if (access("/init.real", X_OK) == 0) return "/init.real";
    if (access("/system/bin/init.real", X_OK) == 0) return "/system/bin/init.real";
    return NULL;
}

/* init 会传给"第二阶段 init"的阶段参数 */
static int is_stage_arg(const char *s)
{
    return !strcmp(s, "selinux_setup") || !strcmp(s, "second_stage") ||
           !strcmp(s, "subcontext");
}

/* 早期注入：在 init 加载策略之前把 BOSS 的规则打进去。
 *
 * 用 **fork + waitpid**，不用 execl 也不直接调用：
 *   · execl 会替换整个进程——一旦成功就再也回不到下面"exec 真实 init"
 *     那一步，表现是开机挂死。交接文档里的示例代码正是这么写的，
 *     照抄就是砖。
 *   · 直接在本进程调用则注入一旦崩溃就没有补救机会。
 *   · fork 之后子进程崩了也只是子进程没了，父进程照常把执行权还给 init。
 * 开机路径上"任何一步失败都必须还能继续开机"，这是本项目的一贯取舍。 */
static int early_sepol_inject(void)
{
    pid_t p = fork();
    if (p < 0) return -1;
    if (p == 0) {
        char *sv[] = { (char *)"selinux", (char *)"setup", NULL };
        _exit(boss_selinux_main(2, sv));
    }

    int st = 0;
    while (waitpid(p, &st, 0) < 0 && errno == EINTR) ;
    if (!WIFEXITED(st)) return -1;

    int rc = WEXITSTATUS(st);
    /* 0 全应用 / 3 部分应用都算成功：策略已经进内核了，
     * 少几条规则只会让某个功能没权限，不该让它拖住开机。 */
    return (rc == 0 || rc == 3) ? 0 : -1;
}

/* 2SI 设备上"谁来跑 post-fs-data 编排"。
 *
 * 问题：veritpath 把 init.boss.rc import 进 **ramdisk** 的 /init.rc，
 * 而 2SI 设备第一阶段结束就 SwitchRoot + chroot，ramdisk 连同那份 rc 一起
 * 消失 —— 第二阶段 init 根本没见过我们的 `on post-fs-data`，于是模块挂载、
 * systemless 清单、脚本在主流机型上一次都不会执行。
 * 这就是"代码写完了但从不生效"的另一半原因（另一半是 hijack-prep 没做）。
 *
 * 解法：被当成第二阶段 init 调起时 fork 一个等待者，等 /data 就绪后自己
 * 触发一次 `boss boot post-fs-data`。不改 /system，不需要 init 认识我们。
 *
 * 取舍：它是**轮询触发**，比 init 原生的 post-fs-data trigger 晚几十毫秒到
 * 几秒（取决于 /data 解密耗时）。对绝大多数模块无影响；对"必须在 init
 * 挂载 /data 那一瞬间生效"的极少数模块，请改走 service.d，别赌这个时序。
 */
static void spawn_boot_waiter(void)
{
    /* 离机验证时不派发等待者。BOSS_INIT_REAL 是 tools/stage2_test.sh 的
     * 测试开关（真机上 init 传下来的环境是空的，它不可能被意外带上）。
     * 不拦这一下的后果：每次跑测试都会在后台留下一个最长 60 秒的轮询进程，
     * 而且它一旦判定 /data 就绪就会真跑一整套 `boss boot post-fs-data`
     * ——在 CI runner 上等于把模块挂载和脚本都执行了一遍。 */
    if (getenv("BOSS_INIT_REAL")) return;

    /* /dev 是 tmpfs：标记随重启消失，天然不会跨开机误判 */
    if (access("/dev/.boss_boot", F_OK) == 0) return;
    int fd = open("/dev/.boss_boot", O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
    if (fd >= 0) close(fd);

    pid_t p = fork();
    if (p != 0) return;                 /* 父进程随后要把执行权还给真实 init */
    setsid();                           /* 独立会话：不被父进程那边的信号波及 */

    /* 判断 /data 就绪的标准就是"落盘能不能成功"——它正是 boot 的第一步。
     * 用这个而不是读某个属性：属性区在这个阶段未必可用。 */
    for (int i = 0; i < 120; i++) {     /* 最多等 60s */
        if (boss_install() == 0) break;
        usleep(500 * 1000);
    }

    const char *bin = access(BOSS_BIN_PATH, X_OK) == 0 ? BOSS_BIN_PATH : NULL;
    if (!bin) _exit(127);
    char *a[] = { (char *)bin, (char *)"boot", (char *)"post-fs-data", NULL };
    execv(bin, a);
    _exit(127);
}

/* v0.2：二阶段劫持入口。
 * 被 SwitchRoot 绑到 /system/bin/init 上后，内核/init 会把我们当 init
 * 执行起来，argv[1] 是阶段参数（selinux_setup / second_stage）。
 *
 * 两种调用约定都要认：
 *   /system/bin/init selinux_setup      阶段参数在 argv[1]（真机 2SI 的写法）
 *   boss init stage2 second_stage       阶段参数在 argv[2]（手动/脚本调用）
 * 以前固定从 argv[2] 开始转发，真机那种写法下阶段参数会被**整个丢掉**——
 * 真实 init 收不到 second_stage 就会重跑 FirstStageMain，直接死循环。
 */
static int cmd_stage2(int argc, char **argv)
{
    /* 阶段参数的起点：argv[1] 是 stage2 本身时从 2 开始，否则就是 1 */
    int start = 1;
    if (argc >= 2 && (!strcmp(argv[1], "stage2") || !strcmp(argv[1], "--stage2")))
        start = 2;

    int is_setup = 0, any_stage = 0;
    for (int i = start; i < argc; i++) {
        if (!strcmp(argv[i], "selinux_setup")) is_setup = 1;
        if (is_stage_arg(argv[i])) any_stage = 1;
    }

    int injected = 0;
    if (is_setup) {
        /* 只有 selinux_setup 这一趟才做早期注入，也只有这一趟不能做别的：
         * 此时 /data 还没解密挂载，boss_install() 往 /data/adb 建目录
         * 要么失败、要么落在还没解密的挂载点上留下垃圾。落盘和拉 daemon
         * 留给随后那一趟 second_stage。 */
        if (cmdline_says_off("boss_selinux=0")) {
            kmsg_log("boss: 早期注入已被 boss_selinux=0 关闭，走原厂路径\n");
        } else {
            injected = (early_sepol_inject() == 0);
            kmsg_log("boss: 早期注入 %s\n", injected ? "成功" : "失败（退回原厂路径）");
        }
    } else {
        boss_install();
        start_daemon();
        /* 2SI 上没有 init 帮我们触发 post-fs-data，自己安排一个等待者 */
        spawn_boot_waiter();
    }

    const char *real = real_init_path();
    if (!real) {
        /* 真实 init 找不到了：这是致命情况，别硬 exec 自己造成死循环 */
        fprintf(stderr, "bossinit: real init not found\n");
        kmsg_log("boss: 找不到真实 init，放弃（绝不 exec 自己）\n");
        return 127;
    }

    char *nargv[8];
    int n = 0;
    nargv[n++] = (char *)real;

    if (is_setup) {
        /* 这一行是整个任务4 的落点：
         *   · 注入成功 → 传 second_stage。策略已经由我们写进内核了，
         *     再让真实 init 跑 selinux_setup，它会拿原始策略再加载一次，
         *     把补丁整个盖掉（症状是"注入明明成功、BOSS 仍在 init 域"）。
         *   · 注入失败 → 传 selinux_setup，让真实 init 走原厂流程。
         *     这样最坏情况只是 BOSS 没有早期规则，开机本身不受影响，
         *     规则会进 pending 等 /data 挂载后由运行时注入兜底。 */
        nargv[n++] = injected ? (char *)"second_stage" : (char *)"selinux_setup";
        kmsg_log("boss: exec %s %s\n", real, nargv[n - 1]);
    } else {
        for (int i = start; i < argc && n < 7; i++) nargv[n++] = argv[i];
        /* 一个阶段参数都没传就 exec 真实 init，它会跑 FirstStageMain——
         * 又是一轮开机循环。既然是当 stage2 调用的，就按 second_stage 兜底。 */
        if (!any_stage) {
            nargv[n++] = (char *)"second_stage";
            kmsg_log("boss: 没有阶段参数，按 second_stage 兜底\n");
        }
    }
    nargv[n] = NULL;

    execv(real, nargv);
    return 127;
}

/* v0.2：为 SwitchRoot 劫持做现场布置。
 *
 * 原理（与 Magisk 同源）：init 的 SwitchRoot 会把 / 下的**挂载**递归移动到
 * /system 再 chroot；我们提前把 /storage/self/primary 指向
 * /system/system/bin/init，再把 bossinit bind 到 /sdcard 上，切根后它就落到
 * /system/bin/init，从而接管第二阶段——全程不写 /system 一个字节。
 *
 * 这一节原来只铺了符号链接，缺两次 mount，等于"布置了现场但没埋雷"。
 * 任务5 补上，因为它是任务5 能不能被执行的前提：
 *   没有 hijack → 没人把我们当第二阶段 init 调起 → cmd_stage2 不跑
 *   → 任务5 的 systemless 编排一次都不会执行。
 * 症状是"代码写完、单测全绿、真机无效果"，排查方向极易跑偏到挂载逻辑本身。
 */
static int cmd_hijack_prep(int argc, char **argv)
{
    /* --dry：只打印将做什么，不真 mount。
     * 两个用处：① 离机测试（沙盒里 mount 既无 SwitchRoot 可观察，也容易把
     * 测试机搞脏）；② 真机排障——先 dry 一遍看它打算挂到哪，再决定放不放手。 */
    int dry = 0;
    for (int i = 0; i < argc; i++) if (!strcmp(argv[i], "--dry")) dry = 1;
    int mounted = 0;

    /* dry run 下连符号链接都不铺：它本来的意义就是"看一眼打算做什么"，
     * 铺了就会在测试机上留下垃圾，反而让人分不清是 dry 留下的还是真跑的。 */
    if (dry) {
        printf("hijack-prep: would mkdir /storage/self\n");
        printf("hijack-prep: would symlink /storage/self/primary -> /system/system/bin/init\n");
    } else {
        (void)boss_mkdirs("/storage/self", 0755);
        (void)unlink("/storage/self/primary");
        if (symlink("/system/system/bin/init", "/storage/self/primary") != 0) { /* 已存在 */ }
    }

    /* 自身路径：真机上就是 ramdisk 里的 /boss（veritpath 放进去的那一份） */
    char self[256] = { 0 };
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n <= 0) boss_copy(self, sizeof(self), "/boss");
    else self[n] = '\0';

    /* ---- ① 备份真实 init（必须在切根之前做） ----
     *
     * 两件事都要做，缺一不可：
     *   a) 硬链接/复制：保住 inode 上的 security.selinux（init_exec 标签）。
     *      任务4 的 6.3 点名过：跳过原厂 selinux_setup 后，init 第二阶段的域
     *      靠 exec 一个带 init_exec 标签的文件完成，标签丢了域就错了。
     *   b) bind mount：ramdisk 里的**文件**在 chroot 后消失，而**挂载**会被
     *      SwitchRoot 移动到 /system 下。所以只做硬链接等于没做——切根后
     *      /init.real 依然不存在，cmd_stage2 走到 execv 前就返回 127。
     */
    if (dry) {
        printf("hijack-prep: would link /init -> /init.real（备份真实 init，切根前必须做完）\n");
    } else if (access("/init.real", F_OK) != 0 && access("/init", F_OK) == 0) {
        if (link("/init", "/init.real") != 0) {
            /* 跨设备或不支持硬链接：退而复制内容（xattr 会丢，阶段 ② 的
             * bind mount 仍保住 inode，所以标签问题由它兜住） */
            int in = open("/init", O_RDONLY | O_CLOEXEC);
            int out = open("/init.real", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
            if (in >= 0 && out >= 0) {
                char buf[65536];
                ssize_t r;
                while ((r = read(in, buf, sizeof(buf))) > 0)
                    if (boss_write_full(out, buf, (size_t)r) < 0) break;
            }
            if (in >= 0) close(in);
            if (out >= 0) close(out);
            chmod("/init.real", 0755);
        }
    }
    if (access("/init.real", F_OK) == 0 && access("/init", F_OK) == 0) {
        if (dry) printf("hijack-prep: would bind /init -> /init.real\n");
        else (void)mount("/init", "/init.real", NULL, MS_BIND, NULL);
    }

    /* ---- ② 主路径：把 bossinit bind 到 /sdcard ----
     * 直接调 mount(2)：第一阶段没有 toolbox，fork /system/bin/mount 必然 127。 */
    if (access("/sdcard", F_OK) != 0) {
        int fd = open("/sdcard", O_CREAT | O_RDONLY | O_CLOEXEC, 0700);
        if (fd >= 0) close(fd);
    }
    if (dry) {
        printf("hijack-prep: would bind %s -> /sdcard\n", self);
        printf("hijack-prep: （dry run：不做任何 mount）\n");
        return 0;
    }
    if (mount(self, "/sdcard", NULL, MS_BIND, NULL) == 0) {
        mounted = 1;
        kmsg_log("boss: hijack-prep bind %s -> /sdcard 成功\n", self);
    } else {
        /* 退路 1：/sdcard 在部分机型上已是一个真实目录（魅族等"用 2SI 但
         * 不切根"的机器），此时挂载点不同，改挂符号链接本身。 */
        if (mount(self, "/storage/self/primary", NULL, MS_BIND, NULL) == 0) {
            mounted = 1;
            kmsg_log("boss: hijack-prep bind -> /storage/self/primary 成功\n");
        } else {
            /* 退路 2：三星 RKP 等不允许新增挂载时，从 rootfs 自挂（/sdcard → /sdcard） */
            if (mount("/sdcard", "/sdcard", NULL, MS_BIND, NULL) == 0) {
                mounted = 1;
                kmsg_log("boss: hijack-prep 自挂 /sdcard 成功（RKP 型设备）\n");
            } else {
                kmsg_log("boss: hijack-prep bind 失败: %s\n", strerror(errno));
            }
        }
    }

    /* ---- ③ 退化路径：rootfs 可写时 hexpatch /init ----
     * 把 init 里 "/system/bin/init" 原地替换成 "/data/bossinit"（等长 16 字节）。
     *
     * 两个前提缺一不可，否则宁可不做：
     *   · /data 必须已挂载可写，否则目标文件不存在 —— init 去 exec 一个
     *     不存在的文件就是变砖，这比"劫持失败"糟得多。
     *   · /init 通常正在执行，open(O_RDWR) 会 ETXTBSY，绝大多数机器上这里
     *     注定失败。它本来就是 Magisk 在**离线 patch boot 镜像**时用的手段，
     *     运行时基本做不到——留着是为了覆盖那些确实可写的老内核。
     */
    if (!mounted && !dry) {
        if (copy_self_to(BOSS_HEXPATCH_PATH) == 0 && hexpatch_init() == 0) {
            mounted = 1;
            kmsg_log("boss: hijack-prep hexpatch /init -> %s 成功\n", BOSS_HEXPATCH_PATH);
        }
    }

    if (!mounted)
        kmsg_log("boss: hijack-prep 全部路径都失败（2SI 设备上 BOSS 不会被调起）\n");

    /* 绝不因为布置失败而让开机停在这里：hijack-prep 失败的原厂行为
     * 就是"正常开机、没有 BOSS"，这是可接受的最坏情况。 */
    return 0;
}

int boss_init_main(int argc, char **argv)
{
    if (argc < 2) {
        /* 兜底：当我们被当成 /init 启动但没有子命令时，
         * 绝不自己充当 init，直接把执行权还给真实 init，避免变砖。 */
        execv("/init.real", argv);
        return 127;
    }

    const char *cmd = argv[1];
    if (!strcmp(cmd, "install") || !strcmp(cmd, "--install"))
        return boss_install() < 0 ? 1 : 0;
    if (!strcmp(cmd, "start") || !strcmp(cmd, "--start") || !strcmp(cmd, "post-fs-data"))
        return cmd_start();
    if (!strcmp(cmd, "stage2") || !strcmp(cmd, "--stage2") || !strcmp(cmd, "second_stage"))
        return cmd_stage2(argc, argv);
    /* init 的第一阶段最后 exec 的是 `/system/bin/init selinux_setup`。
     * 不接这一条，它会落到下面"未知参数"分支原样转发给真实 init——
     * 于是真实 init 用原始策略完成 selinux_setup，我们的早期注入一次
     * 都不会发生（症状是"代码写完了但从不执行"）。 */
    if (!strcmp(cmd, "selinux_setup"))
        return cmd_stage2(argc, argv);
    if (!strcmp(cmd, "hijack-prep") || !strcmp(cmd, "--hijack-prep"))
        return cmd_hijack_prep(argc - 1, argv + 1);
    if (!strcmp(cmd, "alive"))
        return daemon_alive() ? 0 : 1;

    /* 未知参数：同样交还给真实 init */
    execv("/init.real", argv);
    return 127;
}
