/* mntinfo.c — /proc/self/mountinfo 解析
 *
 * 为什么要单独一个文件：任务5 的两件事都建立在同一份事实上。
 *   · systemless verify：证明"改动只来自 /data，没有落 /system"
 *   · hide：找出 BOSS 引入的挂载，把它们从目标进程的视野里摘掉
 * 两边各解析一次 mountinfo 的话，判定标准迟早会漂移（一边认 tmpfs 覆盖、
 * 另一边不认），症状是"verify 说干净、hide 却漏摘一条"这类幽灵 bug。
 *
 * 格式（内核文档 Documentation/filesystems/proc.rst）：
 *   36 35 98:0 /mnt/vendor/persist /mnt/vendor/persist rw,nosuid shared:2 - ext4 /dev/sda1 rw
 *   id parent major:minor root mountpoint options - fstype source superoptions
 *
 * 两处必须注意的坑：
 *   · 挂载点里的空格在内核侧被转义成 \040，这里**不**还原——还原了反而与
 *     mountinfo 里其它字段的字面值对不上。含空格的路径是极少数，遇到就跳过。
 *   · 分隔是 " - "（空格横线空格），直接搜 " -" 会撞上选项里带横线的字段。
 */
#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boss.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* 只读分区名单：verify 拿它当"不该被写"的范围，hide 拿它判断 tmpfs 覆盖层
 * 是不是盖在系统分区上。顺序无关，含结尾 NULL，遍历到 NULL 为止。 */
const char *const boss_ro_parts[] = {
    "/system", "/vendor", "/product", "/system_ext", "/odm", "/odm_dlkm",
    "/vendor_dlkm", NULL
};

/* 路径 p 是否位于 dir 之下（dir 本身也算）。
 * 只做前缀比较 + 边界检查：/systemX 不能被当成 /system 的子路径。 */
static int under(const char *p, const char *dir)
{
    size_t dl = strlen(dir);
    if (strncmp(p, dir, dl) != 0) return 0;
    return p[dl] == '\0' || p[dl] == '/';
}

int boss_mount_is_boss(const struct boss_mount *m)
{
    if (!m) return 0;

    /* 1) 挂载源来自 BOSS 的数据目录：模块文件、镜像目录都是这一类 */
    size_t bl = strlen(BOSS_DIR);
    if (strncmp(m->src, BOSS_DIR, bl) == 0) return 1;

    /* 2) Magic Mount 的覆盖层：tmpfs 盖在只读分区上（module.c 的 mount_node）。
     *    它的 source 字面值是 "tmpfs"，按源判断会漏掉——这恰恰是最该摘掉的一类。 */
    if (!strcmp(m->type, "tmpfs")) {
        for (int i = 0; boss_ro_parts[i]; i++) {
            if (under(m->tgt, boss_ro_parts[i])) return 1;
        }
    }
    return 0;
}

int boss_mount_scan(struct boss_mount ***out)
{
    if (!out) return -1;
    *out = NULL;

    FILE *fp = fopen("/proc/self/mountinfo", "re");
    if (!fp) return -1;

    int cap = 32, n = 0;
    struct boss_mount **v = calloc((size_t)cap, sizeof(*v));
    if (!v) { fclose(fp); return -1; }

    char line[PATH_MAX * 2];
    while (fgets(line, sizeof(line), fp)) {
        char *sep = strstr(line, " - ");
        if (!sep) continue;

        struct boss_mount m;
        memset(&m, 0, sizeof(m));

        /* 前半段：id parent maj:min root mountpoint options */
        char root[512], opts[256], tgt[512];
        root[0] = opts[0] = tgt[0] = '\0';
        *sep = '\0';
        if (sscanf(line, "%d %d %*s %511s %511s %255s",
                   &m.id, &m.parent, root, tgt, opts) != 5)
            continue;
        /* 挂载点含 \040 转义说明路径里有空格，按上面的取舍跳过 */
        if (strstr(tgt, "\\040")) continue;

        /* 后半段：fstype source superoptions */
        char type[64], src[512];
        type[0] = src[0] = '\0';
        if (sscanf(sep + 3, "%63s %511s", type, src) != 2)
            continue;

        boss_copy(m.tgt, sizeof(m.tgt), tgt);
        boss_copy(m.src, sizeof(m.src), src);
        boss_copy(m.type, sizeof(m.type), type);
        boss_copy(m.opts, sizeof(m.opts), opts);

        if (n == cap) {
            int nc = cap * 2;
            struct boss_mount **nv = realloc(v, (size_t)nc * sizeof(*nv));
            if (!nv) break;
            v = nv; cap = nc;
        }
        struct boss_mount *node = calloc(1, sizeof(*node));
        if (!node) break;
        *node = m;
        v[n++] = node;
    }
    fclose(fp);

    *out = v;
    return n;
}
