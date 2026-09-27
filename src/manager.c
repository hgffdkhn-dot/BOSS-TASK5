/* 任务6 · BOSS 客户端（manager）身份识别
 *
 * 为什么需要这个文件：
 *   默认策略是 deny（policy.c 的 kDefaultPolicy）。BOSS App 本身是个普通
 *   app，它的 uid 不可能预先写进 policy.conf——它连 /data/adb/boss 都进不去
 *   （0700 root）。于是出现鸡生蛋问题：**App 必须先被放行，才能写下放行自己
 *   的规则**。这就是任务6 接手时"BOSS 前端授权"那一格空着的真正原因。
 *
 * 解法：daemon 认一个"manager uid"并持久化。
 *   判定用**内核给的 uid**（SO_PEERCRED），cmdline 只做辅助校验——
 *   架构红线第 2 条：身份只信内核。cmdline 是 app 进程由 zygote 填的，
 *   我们拿它当第二道锁，不是为了替代 uid。
 *
 * 残留风险（诚实写出来，别当不存在）：
 *   manager.uid 还没落盘时，任何 **app uid**（>=10000）且 cmdline 等于
 *   BOSS_MANAGER_PKG 的进程都能抢注。窗口是"刷机后第一次开机到 App 首次
 *   联网/启动"之间。真机上想收紧，用 root shell 预先写死：
 *       echo 10123 > /data/adb/boss/manager.uid
 *   或者 `boss policy manager <uid>`。
 */
#define _GNU_SOURCE 1
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "boss.h"

#ifndef BOSS_MANAGER_PKG
#define BOSS_MANAGER_PKG "com.boss.manager"
#endif

#define BOSS_MANAGER_FILE BOSS_DIR "/manager.uid"

/* Android 的 app uid 从 10000 起。低于它的都是系统身份，绝不能当 manager。 */
#define BOSS_APP_UID_MIN 10000u

/* zygote 给 app 进程填的 cmdline 是包名，但后面可能带参数/空格。
 * 只比前缀，且要求后面要么是 NUL 要么是分隔符——否则
 * "com.boss.manager.evil" 会被当成 "com.boss.manager"。 */
static int caller_is_pkg(const char *caller, const char *pkg)
{
    if (!caller || !pkg || !*caller) return 0;
    size_t n = strlen(pkg);
    if (strncmp(caller, pkg, n) != 0) return 0;
    char c = caller[n];
    return (c == '\0' || c == ' ' || c == '\t' || c == ':');
}

int boss_manager_load(uid_t *uid)
{
    if (!uid) return -1;
    FILE *fp = fopen(BOSS_MANAGER_FILE, "re");
    if (!fp) return -1;
    char line[64];
    int ok = 0;
    /* 逐行找第一个不是注释的行。
     * 踩过的坑：文件头写了 "# BOSS manager uid" 这种注释行，解析时
     * strtoul 遇到 '#' 直接返回 0 且 end==line，于是"已注册但读出来是 0"，
     * 表现为 manager 注册成功了、下一次却被判成未注册。
     * 注释是给人看的，解析器必须跳过它——和 policy.conf 一个道理。 */
    while (fgets(line, sizeof(line), fp)) {
        char *s2 = line;
        while (*s2 == ' ' || *s2 == '\t') s2++;
        if (*s2 == '#' || *s2 == '\n' || *s2 == '\0') continue;
        char *end = NULL;
        unsigned long v = strtoul(s2, &end, 10);
        if (end && end != s2 && v >= BOSS_APP_UID_MIN) {
            *uid = (uid_t)v;
            ok = 1;
        }
        break;
    }
    fclose(fp);
    return ok ? 0 : -1;
}

/* 写入/清除 manager uid。uid=0 表示清除（重置，等下次抢注）。 */
int boss_manager_set(uid_t uid)
{
    if (uid == 0) {
        unlink(BOSS_MANAGER_FILE);
        return 0;
    }
    if (boss_mkdirs(BOSS_DIR, 0700) < 0) return -1;
    FILE *fp = fopen(BOSS_MANAGER_FILE, "we");
    if (!fp) return -1;
    fprintf(fp, "# BOSS manager uid（" BOSS_MANAGER_PKG "）\n%u\n", (unsigned)uid);
    fclose(fp);
    chmod(BOSS_MANAGER_FILE, 0644);
    return 0;
}

/* 首次注册。返回 0 = 注册成功，1 = 已有人注册（本次不认），-1 = 不合格。
 * 不管失败原因是什么都不改已有记录——已注册的 manager 不能被顶掉，
 * 否则抢注窗口就从"刷机后一次"变成"每次开机"。 */
int boss_manager_register(uid_t uid, const char *caller)
{
    uid_t cur = 0;
    if (boss_manager_load(&cur) == 0) return 1;
    if (uid < BOSS_APP_UID_MIN) return -1;
    if (!caller_is_pkg(caller, BOSS_MANAGER_PKG)) return -1;
    return boss_manager_set(uid) == 0 ? 0 : -1;
}

/* 这次调用是不是 manager 本人。uid 必须是内核给的那个。 */
int boss_manager_is(uid_t uid, const char *caller)
{
    uid_t cur = 0;
    if (boss_manager_load(&cur) != 0) return 0;
    if ((unsigned)cur != (unsigned)uid) return 0;
    /* cmdline 是辅助校验：uid 对了但包名不对，说明 App 被重装到别的包、
     * 或 uid 被复用给了别的应用——这种情况不放行。 */
    if (caller && !caller_is_pkg(caller, BOSS_MANAGER_PKG)) return 0;
    return 1;
}

const char *boss_manager_pkg(void)
{
    return BOSS_MANAGER_PKG;
}
