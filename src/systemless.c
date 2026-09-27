/* systemless.c — 任务5 A 面：无修改系统逻辑
 *
 * 这一层回答的是一件事：**怎么改系统行为，却不改系统本身。**
 *
 * BOSS 只有两件弹药（任务3 交接文档 4.2 已经划死）：
 *   ① 属性层改写（resetprop）—— 改的是属性区里的值，不是 build.prop 文件
 *   ② 文件层覆盖（module 的 Magic Mount）—— 改的是挂载树，不是分区内容
 * 除了这两件，任何要往 /system 落文件的玩法都是需求越界，本文件也不提供。
 *
 * 那任务5 在这里补的是什么？是三件前辈没做、而"无修改"这面旗子必须有的东西：
 *
 *   1. **声明式**：用户写一份 systemless.conf（prop / delete / persist / deny），
 *      BOSS 按阶段应用，而不是靠一堆散落在 service.d 里的脚本各写各的。
 *      散脚本的问题是"改了什么"没有单一事实来源，也就不可能自检。
 *   2. **时序正确**：post-fs-data 是阻塞阶段，属性改写必须 -n（直写属性区、
 *      不触发 on property:），否则死锁开机（任务3 坑 4 已经栽过一次）。
 *      这里按 --stage 自动决定，不让调用方记这件事。
 *   3. **可自检**：`boss systemless verify` 把"无修改"从口号变成可判定的事实——
 *      读挂载表，断言每一条 BOSS 引入的改动都来自 /data，且只读分区仍为 ro。
 *      自检不了的性质等于没有性质：真机上出问题只能靠猜。
 */
#define _GNU_SOURCE 1
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "boss.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define BOSS_PATH_MAX (PATH_MAX + 64)

/* ------------------------------------------------------------------ */
/* 清单解析                                                            */
/* ------------------------------------------------------------------ */

enum { ST_PROP = 0, ST_PROP_DEL, ST_PERSIST, ST_DENY };

struct sless_entry {
    int  type;
    char key[192];
    char val[512];
    int  lineno;
};

/* 行尾与行首空白都要 strip：属性名带尾随空格会写成 "ro.debuggable "
 * （任务3 坑 3 的原话），症状是"导入了但目标属性没变"。 */
static char *strip(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
        *--e = '\0';
    return s;
}

static int parse_line(char *raw, int lineno, struct sless_entry *e)
{
    char *s = strip(raw);
    if (!*s || *s == '#') return 0;      /* 空行与注释 */

    memset(e, 0, sizeof(*e));
    e->lineno = lineno;

    char *sp = strpbrk(s, " \t");
    char *verb = s;
    char *rest = NULL;
    if (sp) { *sp = '\0'; rest = strip(sp + 1); }

    if (!strcmp(verb, "prop") || !strcmp(verb, "persist")) {
        if (!rest || !*rest) return -1;
        e->type = !strcmp(verb, "persist") ? ST_PERSIST : ST_PROP;
        /* "-d NAME" / "--delete NAME"：删除走同一行式，省得用户记两个关键字 */
        if (!strncmp(rest, "-d ", 3) || !strncmp(rest, "--delete ", 10)) {
            e->type = ST_PROP_DEL;
            rest = strip(rest + (!strncmp(rest, "--delete ", 10) ? 10 : 3));
        }
        char *sp2 = strpbrk(rest, " \t");
        if (sp2) { *sp2 = '\0'; boss_copy(e->val, sizeof(e->val), strip(sp2 + 1)); }
        boss_copy(e->key, sizeof(e->key), rest);
        return *e->key ? 1 : -1;
    }
    if (!strcmp(verb, "deny")) {         /* 顺手把隐藏项也写在同一个清单里 */
        if (!rest || !*rest) return -1;
        e->type = ST_DENY;
        boss_copy(e->key, sizeof(e->key), rest);
        return 1;
    }
    return -1;
}

static int load_conf(const char *path, struct sless_entry **out)
{
    FILE *fp = fopen(path, "re");
    if (!fp) return -1;

    int cap = 16, n = 0;
    struct sless_entry *v = calloc((size_t)cap, sizeof(*v));
    if (!v) { fclose(fp); return -1; }

    char line[1024];
    int lineno = 0;
    while (fgets(line, sizeof(line), fp)) {
        lineno++;
        struct sless_entry e;
        int r = parse_line(line, lineno, &e);
        if (r < 0) {
            /* 坏行只警告不中断：一条写错的规则不该让整份清单失效 */
            fprintf(stderr, "systemless: %s:%d 语法错误，已忽略\n", path, lineno);
            continue;
        }
        if (r == 0) continue;
        if (n == cap) {
            int nc = cap * 2;
            struct sless_entry *nv = realloc(v, (size_t)nc * sizeof(*nv));
            if (!nv) break;
            v = nv; cap = nc;
        }
        v[n++] = e;
    }
    fclose(fp);
    *out = v;
    return n;
}

/* ------------------------------------------------------------------ */
/* 调 resetprop：走 fork/exec，不走内部函数调用                        */
/* ------------------------------------------------------------------ */
/* 理由与 boot.c 的 run_tool 一致：每个组件独立进程，一条属性写崩了
 * （比如属性区布局在某机型上不同）不会带塌整条开机流程。
 * 二进制定位：优先 /data/adb/boss/boss（真机），找不到就退 /proc/self/exe
 * ——离机测试时还没落盘过，没有这个退路冒烟测试全跑不了。 */
static const char *self_bin(void)
{
    static char buf[512];
    if (access(BOSS_BIN_PATH, X_OK) == 0) return BOSS_BIN_PATH;
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) { buf[n] = '\0'; return buf; }
    return NULL;
}

static int run_resetprop(char *const argv[], int *ran)
{
    const char *bin = self_bin();
    if (!bin) return 2;                 /* 2 = 无能力，沿用任务4 的返回码语义 */
    if (ran) *ran = 1;

    pid_t p = fork();
    if (p < 0) return 1;
    if (p == 0) { execv(bin, argv); _exit(127); }

    int st = 0;
    while (waitpid(p, &st, 0) < 0 && errno == EINTR) ;
    if (!WIFEXITED(st)) return 1;
    return WEXITSTATUS(st);
}

/* 属性区目录覆盖（离机测试用）。真机上恒为 NULL：属性区就是 /dev/__properties__。
 * 留这个口子是为了让 apply 也能端到端验——任务3 的 mkprop.py 能造合成属性区，
 * 不接进来就只能验到"命令拼对了"。 */
static const char *g_prop_dir;

/* 一条 prop 条目的执行。noevent = -n（直写属性区） */
static int apply_prop(const struct sless_entry *e, int noevent, int dry)
{
    char *a[10];
    int n = 0;
    a[n++] = (char *)"resetprop";
    if (noevent) a[n++] = (char *)"-n";
    if (g_prop_dir) { a[n++] = (char *)"--dir"; a[n++] = (char *)g_prop_dir; }
    if (e->type == ST_PERSIST) a[n++] = (char *)"-p";
    if (e->type == ST_PROP_DEL) a[n++] = (char *)"-d";
    a[n++] = (char *)e->key;
    if (e->type != ST_PROP_DEL && e->val[0]) a[n++] = (char *)e->val;
    a[n] = NULL;

    if (dry) {
        printf("  prop  %-6s %s", e->type == ST_PERSIST ? "[persist]" :
               e->type == ST_PROP_DEL ? "[-delete]" : "[set]", e->key);
        if (e->type != ST_PROP_DEL && e->val[0]) printf(" = %s", e->val);
        printf("\n");
        return 0;
    }
    return run_resetprop(a, NULL);
}

/* ------------------------------------------------------------------ */
/* 自检：把"无修改"变成可判定的事实                                    */
/* ------------------------------------------------------------------ */

static int opt_has(const char *opts, const char *want)
{
    /* 精确按逗号分隔匹配：子串匹配会把 "errors=remount-ro" 认成 ro */
    if (!opts) return 0;
    const char *p = opts;
    size_t wl = strlen(want);
    for (;;) {
        const char *c = strchr(p, ',');
        size_t len = c ? (size_t)(c - p) : strlen(p);
        if (len == wl && !strncmp(p, want, wl)) return 1;
        if (!c) break;
        p = c + 1;
    }
    return 0;
}

/* 典型 root 痕迹路径：存在不代表是我们干的（厂商也可能带），所以只报告不判失败 */
static const char *const trace_paths[] = {
    "/system/bin/su", "/system/xbin/su", "/sbin/su", "/system/bin/.ext/.su",
    "/system/app/Superuser.apk", "/system/app/SuperSU/SuperSU.apk",
    "/system/etc/init.d", "/su", NULL
};

/* 目录快照：verify 的 --save / --baseline。
 * 只记 路径+大小+mtime，不记内容 hash——真机 /system 上万个文件，全量 sha256
 * 在 post-fs-data 这个阻塞阶段跑不完，而"多文件/少文件/大小变了"已经足够
 * 回答"有没有人往系统分区落东西"。 */
static int snapshot(const char *dir, const char *out)
{
    FILE *fp = fopen(out, "we");
    if (!fp) { fprintf(stderr, "systemless: 无法写 %s: %s\n", out, strerror(errno)); return -1; }

    char cmd[BOSS_PATH_MAX + 64];
    /* 借 find + stat 一把：递归遍历不是本项目的核心能力，别自己写一个 */
    snprintf(cmd, sizeof(cmd),
             "find %s -type f -printf '%%p %%s %%T@\\n' 2>/dev/null | sort", dir);
    FILE *pp = popen(cmd, "re");
    if (!pp) { fclose(fp); return -1; }
    char line[BOSS_PATH_MAX + 64];
    while (fgets(line, sizeof(line), pp)) fputs(line, fp);
    pclose(pp);
    fclose(fp);
    return 0;
}

static int baseline_diff(const char *dir, const char *base)
{
    char cur[BOSS_PATH_MAX + 32];
    snprintf(cur, sizeof(cur), "%s.current", base);
    if (snapshot(dir, cur) < 0) return -1;

    char cmd[BOSS_PATH_MAX + 128];
    snprintf(cmd, sizeof(cmd), "diff -u %s %s | grep -c '^[+-][^+-]' || true", base, cur);
    FILE *pp = popen(cmd, "re");
    int changed = 0;
    if (pp) {
        char line[64] = { 0 };
        if (fgets(line, sizeof(line), pp)) changed = atoi(line);
        pclose(pp);
    }
    unlink(cur);
    return changed;
}

static int cmd_verify(int argc, char **argv)
{
    const char *dir = "/system";
    const char *base = BOSS_SYLESS_BASE;
    int save = 0;

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--dir") && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(argv[i], "--baseline") && i + 1 < argc) base = argv[++i];
        else if (!strcmp(argv[i], "--save")) save = 1;
    }

    if (save) {
        if (snapshot(dir, base) == 0) { printf("基线已写入 %s（%s）\n", base, dir); return 0; }
        return 1;
    }

    printf("BOSS · 无修改自检（systemless verify）\n");
    printf("检查范围：%s\n\n", dir);

    int violations = 0, boss_mounts = 0;

    /* ① 挂载表：每一条 BOSS 引入的改动，源必须来自 /data */
    struct boss_mount **ms = NULL;
    int nm = boss_mount_scan(&ms);
    if (nm > 0) {
        printf("① BOSS 引入的挂载（%d 条之内统计）\n", nm);
        for (int i = 0; i < nm; i++) {
            if (!boss_mount_is_boss(ms[i])) continue;
            boss_mounts++;
            int ok = strncmp(ms[i]->src, "/data/", 6) == 0 ||
                     !strcmp(ms[i]->src, "tmpfs");
            printf("   [%s] %-12s %s -> %s\n", ok ? "OK" : "越界",
                   ms[i]->type, ms[i]->src, ms[i]->tgt);
            if (!ok) violations++;
        }
        if (boss_mounts == 0) printf("   （无：本次开机没有模块挂载或属性覆盖）\n");
        printf("\n");
    } else {
        printf("① 挂载表读取失败——真机上不该发生，请查 /proc 是否可用\n\n");
    }
    for (int i = 0; i < nm; i++) free(ms[i]);
    free(ms);

    /* ② 只读分区是否仍为 ro：被 remount rw 就等于"系统可以被改"，
     *    不管是谁改的，这条都不该为真。 */
    struct boss_mount **ms2 = NULL;
    int nm2 = boss_mount_scan(&ms2);
    printf("② 只读分区状态\n");
    int seen = 0;
    for (int p = 0; boss_ro_parts[p]; p++) {
        for (int i = 0; i < nm2; i++) {
            if (strcmp(ms2[i]->tgt, boss_ro_parts[p])) continue;
            seen++;
            int ro = opt_has(ms2[i]->opts, "ro");
            printf("   %-14s %s\n", boss_ro_parts[p], ro ? "ro（符合预期）" : "rw（越界！）");
            if (!ro) violations++;
            break;
        }
    }
    if (!seen) printf("   （本机没有这些分区：非 Android 或布局不同，跳过）\n");
    printf("\n");
    for (int i = 0; i < nm2; i++) free(ms2[i]);
    free(ms2);

    /* ③ 基线比对（只在明确给了 --dir 且基线存在时才有意义） */
    printf("③ 基线比对\n");
    if (access(base, F_OK) == 0) {
        int changed = baseline_diff(dir, base);
        if (changed < 0) printf("   比对失败\n");
        else if (changed == 0) printf("   与基线一致（%s）\n", base);
        else {
            printf("   与基线有 %d 处差异（%s）\n", changed, base);
            /* 差异不等于越界：模块挂载会让系统分区"看起来"变了。
             * 所以这里只报告，不定罪——定罪要看上面 ① 的挂载源。 */
        }
    } else {
        printf("   无基线。真机上先跑一次：`boss systemless verify --save`\n");
    }

    /* ④ 已知 root 痕迹路径（只报告） */
    printf("\n④ 常见 root 痕迹路径\n");
    int any = 0;
    for (int i = 0; trace_paths[i]; i++) {
        if (access(trace_paths[i], F_OK) == 0) {
            printf("   存在：%s（未必是 BOSS 造成的）\n", trace_paths[i]);
            any = 1;
        }
    }
    if (!any) printf("   无\n");

    printf("\n结论：%s（BOSS 挂载 %d 条，越界 %d 处）\n",
           violations ? "发现越界" : "干净", boss_mounts, violations);
    return violations ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* apply / plan / status                                               */
/* ------------------------------------------------------------------ */

static int cmd_apply(int argc, char **argv, int force_dry)
{
    const char *conf = BOSS_SYLESS_CONF;
    const char *stage = "post-fs-data";
    int dry = force_dry, noevent = -1;

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--file") && i + 1 < argc) conf = argv[++i];
        else if (!strcmp(argv[i], "--stage") && i + 1 < argc) stage = argv[++i];
        else if (!strcmp(argv[i], "--prop-dir") && i + 1 < argc) g_prop_dir = argv[++i];
        else if (!strcmp(argv[i], "-n")) noevent = 1;
        else if (!strcmp(argv[i], "--dry")) dry = 1;
    }
    /* post-fs-data 必须 -n：走 property_service 会死锁开机（任务3 坑 4）。
     * 调用方（boss boot）记不住这件事，所以在这里按阶段兜住。 */
    if (noevent < 0) noevent = !strcmp(stage, "post-fs-data");

    struct sless_entry *es = NULL;
    int n = load_conf(conf, &es);
    if (n < 0) {
        fprintf(stderr, "systemless: 读不到清单 %s（首次使用可复制模板）\n", conf);
        return 1;
    }
    if (n == 0) { printf("清单为空：%s\n", conf); free(es); return 0; }

    printf("应用清单 %s（stage=%s%s）\n", conf, stage, noevent ? "，-n 直写属性区" : "");

    int applied = 0, failed = 0, ran = 0;
    for (int i = 0; i < n; i++) {
        if (es[i].type == ST_DENY) {
            if (dry) { printf("  deny  %s\n", es[i].key); applied++; continue; }
            /* 隐藏项交给 hide 子系统，避免两处各维护一份名单格式 */
            char *a[] = { (char *)"hide", (char *)"denylist", (char *)"add",
                          es[i].key, NULL };
            const char *bin = self_bin();
            if (!bin) { failed++; continue; }
            ran = 1;
            pid_t p = fork();
            if (p == 0) { execv(bin, a); _exit(127); }
            int st = 0;
            while (waitpid(p, &st, 0) < 0 && errno == EINTR) ;
            if (WIFEXITED(st) && WEXITSTATUS(st) == 0) applied++;
            else failed++;
            continue;
        }
        int rc = apply_prop(&es[i], noevent, dry);
        if (rc == 2) { free(es); return 2; }   /* 无能力：连 resetprop 都找不到 */
        ran = 1;
        if (rc == 0) applied++; else failed++;
    }
    free(es);

    if (!ran && !dry) return 2;
    printf("结果：应用 %d 条，失败 %d 条\n", applied, failed);
    /* 返回码语义沿用任务4：0 全应用 / 3 部分应用 / 1 全失败。
     * 部分应用不是错误——一条属性在某机型上不存在不该中断开机。 */
    if (failed == 0) return 0;
    return applied ? 3 : 1;
}

static int cmd_status(void)
{
    printf("BOSS · systemless 状态\n\n");

    struct sless_entry *es = NULL;
    int n = load_conf(BOSS_SYLESS_CONF, &es);
    printf("清单 %s：%s\n", BOSS_SYLESS_CONF, n > 0 ? "已配置" : "未配置/空");
    if (n > 0) {
        int props = 0, denies = 0;
        for (int i = 0; i < n; i++) es[i].type == ST_DENY ? denies++ : props++;
        printf("  属性条目 %d，隐藏条目 %d\n", props, denies);
    }
    free(es);

    struct boss_mount **ms = NULL;
    int nm = boss_mount_scan(&ms);
    int boss_mounts = 0, tmpfs = 0;
    for (int i = 0; i < nm; i++) {
        if (!boss_mount_is_boss(ms[i])) continue;
        boss_mounts++;
        if (!strcmp(ms[i]->type, "tmpfs")) tmpfs++;
    }
    printf("BOSS 引入挂载：%d 条（其中 tmpfs 覆盖层 %d 条）\n", boss_mounts, tmpfs);
    printf("  挂载表总条数：%d\n", nm);
    for (int i = 0; i < nm; i++) free(ms[i]);
    free(ms);

    printf("\n verifies：`boss systemless verify`（断言改动只来自 /data）\n");
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
        "用法: boss systemless <命令> [选项]\n"
        "  apply              应用清单 " BOSS_SYLESS_CONF "\n"
        "      --stage S      post-fs-data（默认，自动 -n）/ service / completed\n"
        "      --file F       指定清单文件\n"
        "      --prop-dir D   覆盖属性区目录（离机测试用，配合 tools/mkprop.py）\n"
        "      --dry          只打印要做的事\n"
        "  plan               apply --dry 的简写\n"
        "  verify             零写入自检（断言改动只来自 /data，只读分区仍为 ro）\n"
        "      --dir D        检查范围（默认 /system）\n"
        "      --baseline F   与基线比对；--save 写基线\n"
        "  status             概览\n\n"
        "清单语法：prop NAME VALUE / prop -d NAME / persist NAME VALUE / deny NAME\n\n"
        "红线：本子系统只做属性改写与挂载覆盖两件事。任何往只读分区落文件的\n"
        "需求都是越界，请打回——'无修改'是靠这条边界成立的。\n");
}

int boss_systemless_main(int argc, char **argv)
{
    /* symlink 调用时 argv[0] 是自己的名字，子命令调用时 applet.c 已偏移好 */
    if (argc >= 1 && argv[0] && !strcmp(argv[0], "systemless")) { argc--; argv++; }
    if (argc < 1) { usage(); return 1; }

    if (!strcmp(argv[0], "apply"))  return cmd_apply(argc - 1, argv + 1, 0);
    if (!strcmp(argv[0], "plan"))   return cmd_apply(argc - 1, argv + 1, 1);
    if (!strcmp(argv[0], "verify")) return cmd_verify(argc - 1, argv + 1);
    if (!strcmp(argv[0], "status")) return cmd_status();

    usage();
    return 1;
}
