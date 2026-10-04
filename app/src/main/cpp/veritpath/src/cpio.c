/* veritpath - Android boot image analyzer and payload injector.
 *
 * _GNU_SOURCE is defined here rather than on the command line so the sources
 * compile identically under -std=c11, -std=gnu11 and any cross toolchain:
 * glibc otherwise hides PATH_MAX, strtok_r, symlink, readlink and lstat.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

/* cpio "newc" archives, with Android multi-segment support. */
#include "compat.h"
#include "vp.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* --------------------------------------------------------- symlink compat
 *
 * MinGW has no symlink()/readlink().  On Windows a symlink is recorded as a
 * regular file whose contents start with VP_LINK_PREFIX, and is turned back
 * into a real symlink wherever one is supported.
 */
#define VP_LINK_PREFIX "veritpath-symlink:"

/* follow symlinks on Windows (no lstat there), do not follow them elsewhere */
static int vp_stat(const char *path, struct stat *st)
{
#ifdef _WIN32
    return stat(path, st);
#else
    return lstat(path, st);
#endif
}

#ifdef _WIN32
static int vp_is_link(const char *full)
{
    (void)full;
    return 0; /* Windows unpack never sees real symlinks in a ramdisk */
}
static int vp_compat_symlink(const char *target, const char *linkpath)
{
    FILE *f = fopen(linkpath, "wb");
    if (!f)
        return -1;
    fprintf(f, "%s%s", VP_LINK_PREFIX, target);
    fclose(f);
    return 0;
}
static ssize_t vp_compat_readlink(const char *full, char *buf, size_t bufsz)
{
    /* a previous unpack on a POSIX host may have left a stub file behind */
    FILE *f = fopen(full, "rb");
    if (!f)
        return -1;
    char tmp[4096];
    size_t n = fread(tmp, 1, sizeof(tmp) - 1, f);
    fclose(f);
    tmp[n] = 0;
    if (strncmp(tmp, VP_LINK_PREFIX, strlen(VP_LINK_PREFIX)) != 0)
        return -1;
    const char *body = tmp + strlen(VP_LINK_PREFIX);
    ssize_t len = (ssize_t)strlen(body);
    if ((size_t)len >= bufsz)
        len = (ssize_t)bufsz - 1;
    memcpy(buf, body, (size_t)len);
    buf[len] = 0;
    return len;
}
#else
static int vp_is_link(const char *full)
{
    struct stat st;
    return lstat(full, &st) == 0 && S_ISLNK(st.st_mode);
}
static int vp_compat_symlink(const char *target, const char *linkpath)
{
    return symlink(target, linkpath);
}
static ssize_t vp_compat_readlink(const char *full, char *buf, size_t bufsz)
{
    return readlink(full, buf, bufsz);
}
#endif


#define CPIO_MAGIC "070701"
#define CPIO_MAGIC_CRC "070702"
#define CPIO_TRAILER "TRAILER!!!"
#define CPIO_HDR 110

static size_t align4(size_t v)
{
    return (v + 3) & ~(size_t)3;
}

static uint32_t hex32(const uint8_t *p)
{
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) {
        char c = (char)p[i];
        uint32_t d;
        if (c >= '0' && c <= '9')
            d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f')
            d = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            d = (uint32_t)(c - 'A' + 10);
        else
            d = 0;
        v = (v << 4) | d;
    }
    return v;
}

static void put_hex32(uint8_t *p, uint32_t v)
{
    static const char *d = "0123456789ABCDEF";
    for (int i = 7; i >= 0; i--) {
        p[i] = (uint8_t)d[v & 0xF];
        v >>= 4;
    }
}

/* strip leading "./" and "/" */
/* Entry names are attacker-controlled (see vp_path_is_safe in util.c). */
static const char *norm_name(const char *n)
{
    while (n[0] == '.' && n[1] == '/')
        n += 2;
    while (*n == '/')
        n++;
    return n;
}

void cpio_init(cpio_archive_t *a)
{
    memset(a, 0, sizeof(*a));
}

void cpio_entry_free(cpio_entry_t *e)
{
    free(e->name);
    buf_free(&e->data);
    memset(e, 0, sizeof(*e));
}

void cpio_free(cpio_archive_t *a)
{
    for (size_t i = 0; i < a->n; i++) {
        cpio_seg_t *s = &a->segs[i];
        for (size_t j = 0; j < s->n; j++)
            cpio_entry_free(&s->entries[j]);
        free(s->entries);
        free(s->label);
        buf_free(&s->gap);
    }
    free(a->segs);
    cpio_init(a);
}

int cpio_entry_set_data(cpio_entry_t *e, const void *data, size_t len)
{
    buf_reset(&e->data);
    return buf_append(&e->data, data, len);
}

static cpio_seg_t *seg_push(cpio_archive_t *a)
{
    if (a->n == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 2;
        a->segs = xrealloc(a->segs, a->cap * sizeof(*a->segs));
    }
    cpio_seg_t *s = &a->segs[a->n++];
    memset(s, 0, sizeof(*s));
    return s;
}

static int entry_push(cpio_seg_t *s, cpio_entry_t e)
{
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 16;
        s->entries = xrealloc(s->entries, s->cap * sizeof(*s->entries));
    }
    s->entries[s->n++] = e;
    return 0;
}

static const char *guess_label(cpio_seg_t *s)
{
    for (size_t i = 0; i < s->n; i++) {
        if (strncmp(s->entries[i].name, "first_stage_ramdisk", 19) == 0)
            return "first_stage_ramdisk";
    }
    for (size_t i = 0; i < s->n; i++) {
        if (strcmp(s->entries[i].name, "init") == 0)
            return "main";
    }
    return "segment";
}

int cpio_parse(const uint8_t *data, size_t len, cpio_archive_t *a)
{
    /* Contract on failure: `a` is left empty and safe to cpio_free().
     * Previously a partially built archive was left behind, and callers that
     * bail out without calling cpio_free() leaked every segment and entry. */
    size_t pos = 0;
    while (pos < len) {
        if (pos + 6 > len)
            break;
        if (memcmp(data + pos, CPIO_MAGIC, 6) != 0 &&
            memcmp(data + pos, CPIO_MAGIC_CRC, 6) != 0) {
            /* skip padding / junk */
            size_t next = pos + 1;
            while (next + 6 <= len && memcmp(data + next, CPIO_MAGIC, 6) != 0)
                next++;
            if (next + 6 > len)
                break;
            pos = next;
        }
        if (pos + CPIO_HDR > len)
            break;
        cpio_seg_t *s = seg_push(a);
        for (;;) {
            if (pos + CPIO_HDR > len)
                goto fail;
            const uint8_t *h = data + pos;
            uint32_t ino = hex32(h + 6);
            uint32_t mode = hex32(h + 14);
            uint32_t uid = hex32(h + 22);
            uint32_t gid = hex32(h + 30);
            uint32_t nlink = hex32(h + 38);
            uint32_t mtime = hex32(h + 46);
            uint32_t fsize = hex32(h + 54);
            uint32_t devmaj = hex32(h + 62);
            uint32_t devmin = hex32(h + 70);
            uint32_t rdevmaj = hex32(h + 78);
            uint32_t rdevmin = hex32(h + 86);
            uint32_t namesize = hex32(h + 94);
            if (namesize < 1 || pos + CPIO_HDR + namesize > len)
                goto fail;
            char *name = xmalloc(namesize);
            memcpy(name, h + CPIO_HDR, namesize - 1);
            name[namesize - 1] = 0;
            size_t dstart = align4(pos + CPIO_HDR + namesize);
            size_t dend = dstart + fsize;
            if (dend > len || dend < dstart) {
                free(name);
                goto fail;
            }
            pos = align4(dend);
            if (strcmp(name, CPIO_TRAILER) == 0) {
                free(name);
                break;
            }
            cpio_entry_t e;
            memset(&e, 0, sizeof(e));
            e.name = name;
            e.mode = mode;
            e.uid = uid;
            e.gid = gid;
            e.nlink = nlink;
            e.mtime = mtime;
            e.devmajor = devmaj;
            e.devminor = devmin;
            e.rdevmajor = rdevmaj;
            e.rdevminor = rdevmin;
            e.ino = ino;
            buf_init(&e.data);
            if (fsize)
                buf_append(&e.data, data + dstart, fsize);
            entry_push(s, e);
        }
        /* gap: zeros until the next archive starts */
        size_t gap_start = pos;
        while (pos < len && data[pos] == 0)
            pos++;
        if (pos < len && memcmp(data + pos, CPIO_MAGIC, 6) == 0) {
            buf_append(&s->gap, data + gap_start, pos - gap_start);
        } else {
            buf_append(&s->gap, data + gap_start, len - gap_start);
            pos = len;
        }
        s->label = xstrdup(guess_label(s));
    }
    if (a->n == 0)
        return -1;
    return 0;

fail:
    cpio_free(a);
    cpio_init(a);
    return -1;
}

static void write_entry(buf_t *out, const cpio_entry_t *e, uint32_t ino)
{
    uint8_t h[CPIO_HDR + 4];
    memcpy(h, CPIO_MAGIC, 6);
    size_t off = 6;
    struct {
        uint32_t v;
    } fields[13];
    fields[0].v = ino;
    fields[1].v = e->mode;
    fields[2].v = e->uid;
    fields[3].v = e->gid;
    fields[4].v = CPIO_IS_DIR(e) ? 2 : (e->nlink ? e->nlink : 1);
    fields[5].v = e->mtime;
    fields[6].v = (uint32_t)e->data.len;
    fields[7].v = e->devmajor;
    fields[8].v = e->devminor;
    fields[9].v = e->rdevmajor;
    fields[10].v = e->rdevminor;
    fields[11].v = (uint32_t)(strlen(e->name) + 1);
    fields[12].v = 0;
    for (int i = 0; i < 13; i++) {
        put_hex32(h + off, fields[i].v);
        off += 8;
    }
    (void)h[CPIO_HDR + 3];
    buf_append(out, h, CPIO_HDR);
    buf_append_str(out, e->name);
    buf_append(out, "\0", 1);
    buf_append_pad(out, 4, 0);
    if (e->data.len)
        buf_append(out, e->data.data, e->data.len);
    buf_append_pad(out, 4, 0);
}

static void write_trailer(buf_t *out)
{
    uint8_t h[CPIO_HDR];
    memcpy(h, CPIO_MAGIC, 6);
    memset(h + 6, '0', 8 * 11);
    put_hex32(h + 6 + 8 * 11, (uint32_t)(strlen(CPIO_TRAILER) + 1));
    put_hex32(h + 6 + 8 * 12, 0);
    buf_append(out, h, CPIO_HDR);
    buf_append_str(out, CPIO_TRAILER);
    buf_append(out, "\0", 1);
    buf_append_pad(out, 4, 0);
}

int cpio_serialize_seg(const cpio_seg_t *seg, buf_t *out)
{
    uint32_t ino = 300000;
    for (size_t i = 0; i < seg->n; i++)
        write_entry(out, &seg->entries[i], ++ino);
    write_trailer(out);
    if (seg->gap.len)
        buf_append(out, seg->gap.data, seg->gap.len);
    return 0;
}

int cpio_serialize(const cpio_archive_t *a, buf_t *out)
{
    for (size_t i = 0; i < a->n; i++)
        cpio_serialize_seg(&a->segs[i], out);
    return 0;
}

cpio_entry_t *cpio_seg_find(cpio_seg_t *seg, const char *name)
{
    const char *n = norm_name(name);
    for (size_t i = 0; i < seg->n; i++) {
        if (strcmp(norm_name(seg->entries[i].name), n) == 0)
            return &seg->entries[i];
    }
    return NULL;
}

cpio_entry_t *cpio_find(cpio_archive_t *a, const char *name)
{
    for (size_t i = 0; i < a->n; i++) {
        cpio_entry_t *e = cpio_seg_find(&a->segs[i], name);
        if (e)
            return e;
    }
    return NULL;
}

int cpio_add(cpio_archive_t *a, size_t seg, const cpio_entry_t *entry)
{
    if (a->n == 0)
        seg_push(a);
    if (seg >= a->n)
        seg = a->n - 1;
    cpio_seg_t *s = &a->segs[seg];
    cpio_entry_t *old = cpio_seg_find(s, entry->name);
    if (old) {
        /* Copy, exactly like the insert path below. This used to steal the
         * caller's name/data, so replacing an existing entry (payload init over
         * a skeleton /init, say) left the caller to free them a second time -
         * a use-after-free ASAN catches immediately. */
        cpio_entry_free(old);
        cpio_entry_t copy = *entry;
        copy.name = xstrdup(entry->name);
        buf_init(&copy.data);
        buf_append(&copy.data, entry->data.data, entry->data.len);
        *old = copy;
        return 1; /* replaced */
    }
    cpio_entry_t copy = *entry;
    copy.name = xstrdup(entry->name);
    buf_init(&copy.data);
    buf_append(&copy.data, entry->data.data, entry->data.len);
    entry_push(s, copy);
    return 0;
}

int cpio_ensure_dir(cpio_archive_t *a, size_t seg, const char *path)
{
    const char *p = norm_name(path);
    char buf[512];
    size_t cap = sizeof(buf);
    for (size_t i = 0; p[i]; i++) {
        if (p[i] != '/' || i == 0)
            continue;
        size_t n = i < cap ? i : cap - 1;
        memcpy(buf, p, n);
        buf[n] = 0;
        if (!cpio_find(a, buf)) {
            cpio_entry_t e;
            memset(&e, 0, sizeof(e));
            e.name = xstrdup(buf);
            e.mode = 0040755;
            e.nlink = 2;
            cpio_add(a, seg, &e);
            free(e.name);
            buf_free(&e.data);
        }
    }
    return 0;
}

size_t cpio_main_segment(cpio_archive_t *a)
{
    if (a->n == 0)
        return 0;
    for (size_t i = 0; i < a->n; i++) {
        if (a->segs[i].label && strcmp(a->segs[i].label, "main") == 0)
            return i;
    }
    return a->n - 1;
}

/* ------------------------------------------------------- directory layout */

int cpio_extract_dir(cpio_archive_t *a, const char *root)
{
    for (size_t i = 0; i < a->n; i++) {
        cpio_seg_t *s = &a->segs[i];
        char *base;
        if (a->n == 1) {
            base = xstrdup(root);
        } else {
            char segname[32];
            snprintf(segname, sizeof(segname), "segment%zu", i);
            base = path_join(root, segname);
        }
        mkdir_p(base);
        char *label = path_join(base, ".segment-label");
        FILE *f = fopen(label, "w");
        if (f) {
            fprintf(f, "%s\n", s->label ? s->label : "segment");
            fclose(f);
        }
        free(label);
        for (size_t j = 0; j < s->n; j++) {
            cpio_entry_t *e = &s->entries[j];
            const char *rel = norm_name(e->name);
            if (!vp_path_is_safe(rel)) {
                vp_warn("refusing an entry that escapes the output directory: "
                        "%s", e->name);
                continue;
            }
            char *target = path_join(base, rel);
            char *parent = xstrdup(target);
            char *slash = strrchr(parent, '/');
            if (slash) {
                *slash = 0;
                mkdir_p(parent);
            }
            free(parent);
            if (CPIO_IS_DIR(e)) {
                /* do not let a symlink planted by an earlier entry redirect
                 * this directory outside the tree */
                if (vp_is_link(target))
                    unlink(target);
                mkdir_p(target);
            } else if (CPIO_IS_LINK(e)) {
                /* A symlink target is just a string and absolute targets are
                 * normal in an Android ramdisk (/init -> /system/bin/init), so
                 * they are allowed. What must not happen is a later entry
                 * being written *through* a symlink, which is handled below
                 * by unlinking the path before writing. */
                unlink(target);
                const char *lt = (const char *)e->data.data;
                if (vp_compat_symlink(lt, target) != 0)
                    vp_warn("cannot create symlink %s", target);
            } else {
                /* same reason: fopen() follows a symlink, so drop one first */
                if (vp_is_link(target))
                    unlink(target);
                FILE *of = fopen(target, "wb");
                if (of) {
                    if (e->data.len)
                        fwrite(e->data.data, 1, e->data.len, of);
                    fclose(of);
                    chmod(target, (mode_t)CPIO_PERMS(e));
                }
            }
            free(target);
        }
        free(base);
    }
    return 0;
}

static int collect_dir(const char *root, const char *base, cpio_seg_t *seg);

static void add_file_entry(cpio_seg_t *seg, const char *rel, const char *full,
                           const struct stat *st, int is_link)
{
    cpio_entry_t e;
    memset(&e, 0, sizeof(e));
    e.name = xstrdup(rel);
    e.uid = st->st_uid;
    e.gid = st->st_gid;
    e.mtime = (uint32_t)st->st_mtime;
    e.nlink = 1;
    if (is_link) {
        e.mode = 0120777;
        char target[PATH_MAX];
        ssize_t n = vp_compat_readlink(full, target, sizeof(target) - 1);
        if (n > 0) {
            target[n] = 0;
            buf_append(&e.data, target, (size_t)n);
        }
    } else {
        e.mode = (uint32_t)(st->st_mode & 07777) | 0100000;
        buf_t data;
        buf_init(&data);
        if (read_file(full, &data) == 0)
            e.data = data;
    }
    entry_push(seg, e);
}

static int collect_dir(const char *root, const char *base, cpio_seg_t *seg)
{
    DIR *d = opendir(base);
    if (!d)
        return -1;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        char *full = path_join(base, de->d_name);
        struct stat st;
        int is_link = 0;
        is_link = vp_is_link(full);
        if (vp_stat(full, &st) != 0) {
            free(full);
            continue;
        }
        const char *rel = full + strlen(root);
        while (*rel == '/')
            rel++;
        if (strcmp(rel, ".segment-label") == 0) {
            free(full);
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            cpio_entry_t e;
            memset(&e, 0, sizeof(e));
            e.name = xstrdup(rel);
            e.mode = 0040755;
            e.nlink = 2;
            entry_push(seg, e);
            collect_dir(root, full, seg);
        } else if (S_ISLNK(st.st_mode)) {
            is_link = 1;
            add_file_entry(seg, rel, full, &st, is_link);
        } else if (S_ISREG(st.st_mode)) {
            add_file_entry(seg, rel, full, &st, 0);
        }
        free(full);
    }
    closedir(d);
    return 0;
}

int cpio_build_dir(const char *root, cpio_archive_t *a)
{
    /* does the directory contain segmentN/ sub dirs? */
    DIR *d = opendir(root);
    if (!d)
        return -1;
    struct dirent *de;
    int n_segdirs = 0;
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, "segment", 7) == 0) {
            char *full = path_join(root, de->d_name);
            if (is_dir(full))
                n_segdirs++;
            free(full);
        }
    }
    closedir(d);
    if (n_segdirs == 0) {
        cpio_seg_t *s = seg_push(a);
        s->label = xstrdup("main");
        return collect_dir(root, root, s);
    }
    d = opendir(root);
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, "segment", 7) != 0)
            continue;
        char *full = path_join(root, de->d_name);
        if (!is_dir(full)) {
            free(full);
            continue;
        }
        cpio_seg_t *s = seg_push(a);
        char *label = path_join(full, ".segment-label");
        FILE *f = fopen(label, "r");
        char lbl[64] = "segment";
        if (f) {
            if (fgets(lbl, sizeof(lbl), f)) {
                size_t n = strlen(lbl);
                while (n && (lbl[n - 1] == '\n' || lbl[n - 1] == '\r'))
                    lbl[--n] = 0;
            }
            fclose(f);
        }
        s->label = xstrdup(lbl[0] ? lbl : "segment");
        free(label);
        collect_dir(full, full, s);
        free(full);
    }
    closedir(d);
    return 0;
}

/* --------------------------------------------------------- ramdisk skeleton
 *
 * Some devices boot with no ramdisk at all: system-as-root mounts /system as /
 * and the kernel runs /system/bin/init straight from there, so boot.img carries
 * only a kernel and a dtb. To patch such a device you have to *create* a
 * ramdisk, because there is nothing to patch.
 *
 * This builds the directory tree and the files every Android first-stage init
 * expects. It does NOT contain a working init - see cpio_create_skeleton().
 */
#define VP_PLACEHOLDER_MARKER "veritpath-placeholder-init"

static void skel_dir(cpio_archive_t *a, size_t seg, const char *path)
{
    if (cpio_find(a, path))
        return;
    cpio_entry_t e;
    memset(&e, 0, sizeof(e));
    e.name = xstrdup(path);
    e.mode = 0040755;
    e.nlink = 2;
    buf_init(&e.data);
    cpio_add(a, seg, &e);
    free(e.name);
}

static void skel_file(cpio_archive_t *a, size_t seg, const char *path,
                      unsigned mode, const char *content)
{
    cpio_entry_t e;
    memset(&e, 0, sizeof(e));
    e.name = xstrdup(path);
    e.mode = mode;
    e.nlink = 1;
    buf_init(&e.data);
    buf_append(&e.data, content, strlen(content));
    cpio_add(a, seg, &e);
    free(e.name);
    buf_free(&e.data);
}

int cpio_create_skeleton(cpio_archive_t *a)
{
    static const char *dirs[] = {
        "/dev", "/proc", "/sys", "/system", "/data", "/mnt", "/apex",
        "/debug_ramdisk", "/storage", "/acct", "/config", "/cache",
        "/metadata", "/second_stage_resources",
    };
    skel_dir(a, 0, "/");
    /* note: cpio_ensure_dir() only creates the *parents* of a path, so the
     * top-level mount points have to be added explicitly */
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
        skel_dir(a, 0, dirs[i]);

    /* init.rc is what makes the payload reachable: Android's init imports
     * whatever is listed here before running its own sections. */
    skel_file(a, 0, "/init.rc", 0100644,
              "# veritpath skeleton init.rc\n"
              "# Imported first so the payload is set up before init proceeds.\n"
              "import /init.veritpath.rc\n"
              "\n"
              "on early-init\n"
              "    mkdir /dev 0755\n"
              "    mount proc proc /proc\n"
              "    mount sysfs sysfs /sys\n"
              "\n"
              "on init\n"
              "    mkdir /system 0755\n");

    /* empty but present: init fails to start without a file_contexts */
    skel_file(a, 0, "/file_contexts", 0100644,
              "# veritpath skeleton: add your own labels here\n");

    skel_file(a, 0, "/init", 0100755,
              "#!/system/bin/sh\n"
              "# " VP_PLACEHOLDER_MARKER "\n"
              "#\n"
              "# PLACEHOLDER - this will NOT boot a device.\n"
              "#\n"
              "# This ramdisk was created by veritpath because the boot.img carried\n"
              "# none. At first-stage init there is no /system yet, so the shell\n"
              "# above does not exist either - this file only documents the shape.\n"
              "#\n"
              "# Replace /init with a real static first-stage init (your own, or\n"
              "# magiskinit). It has to:\n"
              "#   1. do whatever your payload needs\n"
              "#   2. mount /system (system-as-root: the kernel does not do it)\n"
              "#   3. exec the original init, normally /system/bin/init\n"
              "#\n"
              "# veritpath inject -p payload --create-ramdisk  drops your init in\n"
              "# place of this file if the payload declares dest \"/init\".\n");

    skel_file(a, 0, "/veritpath-skeleton.txt", 0100644,
              "This ramdisk was created by veritpath because the image carried\n"
              "none (system-as-root device).\n"
              "\n"
              "Replace /init with a real static first-stage init before flashing.\n"
              "The original init lives on /system (usually /system/bin/init).\n");
    return 0;
}

int cpio_has_placeholder_init(cpio_archive_t *a)
{
    cpio_entry_t *e = cpio_find(a, "init");
    if (!e)
        return 1;               /* no init at all is even worse */
    return vp_memmem(e->data.data, e->data.len, VP_PLACEHOLDER_MARKER,
                     strlen(VP_PLACEHOLDER_MARKER)) != NULL;
}
