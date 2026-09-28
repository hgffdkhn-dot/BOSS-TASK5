/* veritpath - Android boot image analyzer and payload injector.
 *
 * _GNU_SOURCE is defined here rather than on the command line so the sources
 * compile identically under -std=c11, -std=gnu11 and any cross toolchain:
 * glibc otherwise hides PATH_MAX, strtok_r, symlink, readlink and lstat.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

/* Payload loading (manifest.json) and ramdisk injection. */
#include "vp.h"

#include <dirent.h>
#include <stdlib.h>
#include <sys/stat.h>

#define RC_TEMPLATE                                                            \
    "# injected by veritpath (payload: %s)\n"                                  \
    "on early-init\n"                                                          \
    "    export VERITPATH_PAYLOAD %s\n"                                        \
    "\n"                                                                       \
    "on post-fs-data\n"                                                        \
    "    exec - root root -- /su --install\n"                                  \
    "\n"                                                                       \
    "service veritpath-%s /su --daemon\n"                                      \
    "    class late_start\n"                                                   \
    "    user root\n"                                                          \
    "    group root\n"                                                         \
    "    seclabel u:r:init:s0\n"                                               \
    "    oneshot\n"

static void rc_init(payload_rc_t *rc)
{
    memset(rc, 0, sizeof(*rc));
    snprintf(rc->file, sizeof(rc->file), "/init.veritpath.rc");
}

static int parse_mode(const char *s)
{
    if (!s)
        return 0755;
    if (s[0] == '0' && strlen(s) <= 4) {
        int v = 0;
        for (const char *p = s; *p; p++) {
            if (*p < '0' || *p > '7')
                return 0755;
            v = v * 8 + (*p - '0');
        }
        return v;
    }
    return atoi(s);
}

void payload_free(payload_t *p)
{
    free(p->rc.content);
    for (int i = 0; i < p->rc.n_import; i++)
        free(p->rc.import_into[i]);
}

static int add_file(payload_t *p, const char *src, const char *dest, int mode,
                    const char *ctx, const char *backup)
{
    if (p->n_files >= VP_MAX_FILES)
        return -1;
    payload_file_t *f = &p->files[p->n_files++];
    memset(f, 0, sizeof(*f));
    snprintf(f->src, sizeof(f->src), "%s", src);
    snprintf(f->dest, sizeof(f->dest), "%s", dest);
    f->mode = mode;
    f->required = 1;
    if (ctx)
        snprintf(f->context, sizeof(f->context), "%s", ctx);
    if (backup)
        snprintf(f->backup_as, sizeof(f->backup_as), "%s", backup);
    return 0;
}

static int load_manifest(const char *path, payload_t *p)
{
    buf_t text;
    buf_init(&text);
    if (read_file(path, &text) != 0) {
        buf_free(&text);
        return -1;
    }
    json_val *root = NULL;
    if (json_parse((const char *)text.data, &root) != 0 || !root) {
        buf_free(&text);
        return -1;
    }
    snprintf(p->name, sizeof(p->name), "%s", json_get_str(root, "name", "payload"));
    snprintf(p->version, sizeof(p->version), "%s", json_get_str(root, "version", "0.0.0"));
    p->min_api = (int)json_get_num(root, "min_api", 0);
    p->max_api = (int)json_get_num(root, "max_api", 0);
    snprintf(p->selinux, sizeof(p->selinux), "%s", json_get_str(root, "selinux", "keep"));

    json_val *arch = json_get(root, "arch");
    if (arch && arch->type == J_ARR) {
        for (size_t i = 0; i < arch->n && p->n_arch < VP_MAX_ARCH; i++) {
            if (arch->items[i]->type == J_STR)
                snprintf(p->arch[p->n_arch++], 16, "%s", arch->items[i]->str);
        }
    }

    json_val *files = json_get(root, "files");
    if (files && files->type == J_ARR) {
        for (size_t i = 0; i < files->n && p->n_files < VP_MAX_FILES; i++) {
            json_val *e = files->items[i];
            if (!e || e->type != J_OBJ)
                continue;
            payload_file_t *f = &p->files[p->n_files++];
            memset(f, 0, sizeof(*f));
            snprintf(f->src, sizeof(f->src), "%s", json_get_str(e, "src", ""));
            snprintf(f->dest, sizeof(f->dest), "%s", json_get_str(e, "dest", ""));
            f->mode = parse_mode(json_get_str(e, "mode", "0755"));
            f->uid = (int)json_get_num(e, "uid", 0);
            f->gid = (int)json_get_num(e, "gid", 0);
            snprintf(f->context, sizeof(f->context), "%s", json_get_str(e, "context", ""));
            snprintf(f->backup_as, sizeof(f->backup_as), "%s",
                     json_get_str(e, "backup_as", ""));
            snprintf(f->symlink, sizeof(f->symlink), "%s", json_get_str(e, "symlink", ""));
            f->required = json_get_bool(e, "required", 1);
        }
    }

    json_val *rc = json_get(root, "rc");
    if (rc && rc->type == J_OBJ) {
        p->has_rc = 1;
        snprintf(p->rc.file, sizeof(p->rc.file), "%s",
                 json_get_str(rc, "file", "/init.veritpath.rc"));
        const char *content = json_get_str(rc, "content", "");
        if (content && *content)
            p->rc.content = xstrdup(content);
        const char *cf = json_get_str(rc, "content_file", "");
        if (cf && *cf) {
            char *full = path_join(p->root, cf);
            buf_t b;
            buf_init(&b);
            if (read_file(full, &b) == 0) {
                free(p->rc.content);
                p->rc.content = xstrdup((const char *)b.data);
            }
            buf_free(&b);
            free(full);
        }
        const char *append_to = json_get_str(rc, "append_to", "");
        if (append_to && *append_to)
            snprintf(p->rc.append_to, sizeof(p->rc.append_to), "%s", append_to);
        json_val *imp = json_get(rc, "import_into");
        if (imp && imp->type == J_ARR) {
            for (size_t i = 0; i < imp->n && p->rc.n_import < VP_MAX_IMPORT; i++) {
                if (imp->items[i]->type == J_STR)
                    p->rc.import_into[p->rc.n_import++] = xstrdup(imp->items[i]->str);
            }
        }
    }

    json_val *cmd = json_get(root, "cmdline_append");
    if (cmd && cmd->type == J_ARR) {
        for (size_t i = 0; i < cmd->n; i++) {
            if (cmd->items[i]->type != J_STR)
                continue;
            if (p->cmdline_append[0])
                strncat(p->cmdline_append, " ",
                        sizeof(p->cmdline_append) - strlen(p->cmdline_append) - 1);
            strncat(p->cmdline_append, cmd->items[i]->str,
                    sizeof(p->cmdline_append) - strlen(p->cmdline_append) - 1);
        }
    }
    json_free(root);
    buf_free(&text);
    return 0;
}

static int scan_dir(payload_t *p)
{
    DIR *d = opendir(p->root);
    if (!d)
        return -1;
    struct dirent *de;
    int has_su = 0;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.')
            continue;
        char *full = path_join(p->root, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
            free(full);
            continue;
        }
        if (strcmp(de->d_name, "manifest.json") == 0 ||
            strcmp(de->d_name, "veritpath.json") == 0) {
            free(full);
            continue;
        }
        if (strcmp(de->d_name, "su") == 0 || strncmp(de->d_name, "su.", 3) == 0) {
            add_file(p, de->d_name, "/su", 0755, "u:object_r:rootfs:s0", NULL);
            has_su = 1;
        } else if (strcmp(de->d_name, "init") == 0) {
            add_file(p, de->d_name, "/init", 0755, NULL, "/init.real");
        } else {
            const char *ext = strrchr(de->d_name, '.');
            if (ext && strcmp(ext, ".rc") == 0) {
                char dest[512];
                snprintf(dest, sizeof(dest), "/%s", de->d_name);
                add_file(p, de->d_name, dest, 0644, NULL, NULL);
                p->has_rc = 1;
                snprintf(p->rc.file, sizeof(p->rc.file), "/%.200s", de->d_name);
                free(p->rc.content);
                buf_t b;
                buf_init(&b);
                read_file(full, &b);
                p->rc.content = xstrdup((const char *)b.data);
                buf_free(&b);
                if (p->rc.n_import < VP_MAX_IMPORT)
                    p->rc.import_into[p->rc.n_import++] = xstrdup("/init.rc");
            } else {
                char dest[512];
                snprintf(dest, sizeof(dest), "/veritpath/%.200s", de->d_name);
                add_file(p, de->d_name, dest, 0755, NULL, NULL);
            }
        }
        free(full);
    }
    closedir(d);
    if (has_su && !p->has_rc) {
        p->has_rc = 1;
        char *rc = xmalloc(1024);
        snprintf(rc, 1024, RC_TEMPLATE, p->name, p->name, p->name);
        p->rc.content = rc;
        if (p->rc.n_import < VP_MAX_IMPORT)
            p->rc.import_into[p->rc.n_import++] = xstrdup("/init.rc");
    }
    return 0;
}

int payload_load(const char *dir, payload_t *p)
{
    memset(p, 0, sizeof(*p));
    rc_init(&p->rc);
    snprintf(p->name, sizeof(p->name), "%s", dir);
    snprintf(p->version, sizeof(p->version), "0.0.0");
    snprintf(p->selinux, sizeof(p->selinux), "keep");
    snprintf(p->root, sizeof(p->root), "%s", dir);
    const char *base = strrchr(dir, '/');
    base = base ? base + 1 : dir;
    if (*base)
        snprintf(p->name, sizeof(p->name), "%s", base);

    char *mf = path_join(dir, "manifest.json");
    if (file_exists(mf)) {
        if (load_manifest(mf, p) != 0) {
            free(mf);
            return -1;
        }
    } else {
        free(mf);
        mf = path_join(dir, "veritpath.json");
        if (file_exists(mf)) {
            if (load_manifest(mf, p) != 0) {
                free(mf);
                return -1;
            }
        } else if (scan_dir(p) != 0) {
            free(mf);
            return -1;
        }
    }
    free(mf);

    /* validate */
    for (int i = 0; i < p->n_files; i++) {
        payload_file_t *f = &p->files[i];
        if (f->dest[0] != '/') {
            vp_err("payload file '%s': dest must be an absolute ramdisk path", f->src);
            return -1;
        }
        if (f->required) {
            char *full = path_join(p->root, f->src);
            if (!file_exists(full)) {
                vp_err("payload '%s' misses required file: %s", p->name, f->src);
                free(full);
                return -1;
            }
            free(full);
        }
    }
    return 0;
}

int payload_check(payload_t *p, const char *arch, int api, char *msg, size_t msgz)
{
    msg[0] = 0;
    int bad = 0;
    if (p->n_arch && arch && strcmp(arch, "unknown") != 0) {
        int ok = 0;
        for (int i = 0; i < p->n_arch; i++) {
            if (strcmp(p->arch[i], arch) == 0)
                ok = 1;
        }
        if (!ok) {
            snprintf(msg, msgz, "payload supports %s but the image is %s",
                     p->arch[0], arch);
            bad = 1;
        }
    }
    if (p->min_api && api && api < p->min_api) {
        snprintf(msg, msgz, "payload needs API>=%d, image is API %d", p->min_api, api);
        bad = 1;
    }
    if (p->max_api && api && api > p->max_api) {
        snprintf(msg, msgz, "payload supports up to API %d, image is %d", p->max_api, api);
        bad = 1;
    }
    return bad;
}

static void result_add(char list[][256], int *n, int cap, const char *name)
{
    for (int i = 0; i < *n; i++) {
        if (strcmp(list[i], name) == 0)
            return;
    }
    if (*n >= cap)
        return;
    snprintf(list[(*n)++], 256, "%s", name);
}

static void apply_rc(cpio_archive_t *a, size_t seg, payload_t *p, inject_result_t *r)
{
    (void)r;
    if (!p->has_rc || !p->rc.content)
        return;
    cpio_entry_t e;
    memset(&e, 0, sizeof(e));
    e.name = xstrdup(p->rc.file[0] == '/' ? p->rc.file + 1 : p->rc.file);
    e.mode = 0100644;
    buf_append(&e.data, p->rc.content, strlen(p->rc.content));
    if (!e.data.len || e.data.data[e.data.len - 1] != '\n')
        buf_append(&e.data, "\n", 1);
    cpio_add(a, seg, &e);
    result_add(r->added, &r->n_added, VP_MAX_FILES * 2, p->rc.file);
    free(e.name);
    buf_free(&e.data);

    if (p->rc.append_to[0]) {
        cpio_entry_t *t = cpio_find(a, p->rc.append_to);
        if (t) {
            if (!vp_memmem(t->data.data, t->data.len, p->rc.content, strlen(p->rc.content)))
                buf_append_str(&t->data, p->rc.content);
        }
        return;
    }
    int n = p->rc.n_import;
    if (n == 0) {
        static const char *def[] = {"/init.rc"};
        for (int i = 0; i < 1; i++) {
            cpio_entry_t *t = cpio_find(a, def[i]);
            if (!t) {
                vp_warn("rc target %s not found in ramdisk - creating it", def[i]);
                cpio_entry_t ne;
                memset(&ne, 0, sizeof(ne));
                ne.name = xstrdup(def[i] + 1);
                ne.mode = 0100644;
                buf_append_str(&ne.data, p->rc.content);
                cpio_add(a, seg, &ne);
                free(ne.name);
                buf_free(&ne.data);
                continue;
            }
            char line[300];
            snprintf(line, sizeof(line), "\nimport %s\n", p->rc.file);
            if (!vp_memmem(t->data.data, t->data.len, p->rc.file, strlen(p->rc.file)))
                buf_append_str(&t->data, line);
        }
        return;
    }
    for (int i = 0; i < n; i++) {
        cpio_entry_t *t = cpio_find(a, p->rc.import_into[i]);
        if (!t) {
            vp_warn("rc target %s not found in ramdisk - creating it",
                    p->rc.import_into[i]);
            cpio_entry_t ne;
            memset(&ne, 0, sizeof(ne));
            ne.name = xstrdup(p->rc.import_into[i][0] == '/'
                                  ? p->rc.import_into[i] + 1
                                  : p->rc.import_into[i]);
            ne.mode = 0100644;
            buf_append_str(&ne.data, p->rc.content);
            cpio_add(a, seg, &ne);
            free(ne.name);
            buf_free(&ne.data);
            continue;
        }
        char line[300];
        snprintf(line, sizeof(line), "\nimport %s\n", p->rc.file);
        if (!vp_memmem(t->data.data, t->data.len, p->rc.file, strlen(p->rc.file)))
            buf_append_str(&t->data, line);
    }
}

static const char *file_contexts_candidates[] = {
    "/file_contexts", "/plat_file_contexts", "/sepolicy_contexts"};

int payload_apply(cpio_archive_t *a, size_t seg, payload_t *p, inject_result_t *r,
                  int selinux_permissive, const char *extra_cmdline)
{
    (void)selinux_permissive;
    (void)extra_cmdline;
    for (int i = 0; i < p->n_files; i++) {
        payload_file_t *f = &p->files[i];
        char *full = path_join(p->root, f->src);
        if (!file_exists(full)) {
            if (f->required) {
                vp_err("missing payload file: %s", full);
                free(full);
                return -1;
            }
            free(full);
            continue;
        }
        buf_t data;
        buf_init(&data);
        read_file(full, &data);
        free(full);

        const char *dest = f->dest[0] == '/' ? f->dest + 1 : f->dest;
        cpio_ensure_dir(a, seg, dest);

        cpio_entry_t *old = cpio_find(a, dest);
        if (old) {
            result_add(r->replaced, &r->n_replaced, VP_MAX_FILES, f->dest);
            if (f->backup_as[0]) {
                cpio_entry_t backup = *old;
                backup.name = xstrdup(f->backup_as[0] == '/' ? f->backup_as + 1
                                                             : f->backup_as);
                buf_init(&backup.data);
                buf_append(&backup.data, old->data.data, old->data.len);
                cpio_add(a, seg, &backup);
                free(backup.name);
                buf_free(&backup.data);
                result_add(r->backed_up, &r->n_backed, VP_MAX_FILES, f->backup_as);
            }
        } else {
            result_add(r->added, &r->n_added, VP_MAX_FILES * 2, f->dest);
        }

        cpio_entry_t e;
        memset(&e, 0, sizeof(e));
        e.name = xstrdup(dest);
        e.uid = (uint32_t)f->uid;
        e.gid = (uint32_t)f->gid;
        if (f->symlink[0]) {
            e.mode = 0120777;
            buf_append_str(&e.data, f->symlink);
        } else {
            e.mode = (uint32_t)f->mode | 0100000;
            e.data = data;
        }
        cpio_add(a, seg, &e);
        free(e.name);
        if (f->symlink[0])
            buf_free(&e.data);
        else
            buf_free(&e.data);
    }

    apply_rc(a, seg, p, r);

    /* SELinux labels */
    int need_ctx = 0;
    for (int i = 0; i < p->n_files && !need_ctx; i++)
        need_ctx = p->files[i].context[0] != 0;
    if (need_ctx) {
        cpio_entry_t *fc = NULL;
        for (size_t i = 0; i < sizeof(file_contexts_candidates) / sizeof(char *); i++) {
            fc = cpio_find(a, file_contexts_candidates[i]);
            if (fc)
                break;
        }
        if (fc) {
            for (int i = 0; i < p->n_files; i++) {
                if (!p->files[i].context[0])
                    continue;
                char line[400];
                snprintf(line, sizeof(line), "%s %s", p->files[i].dest,
                         p->files[i].context);
                if (!vp_memmem(fc->data.data, fc->data.len, line, strlen(line))) {
                    buf_append_str(&fc->data, line);
                    buf_append(&fc->data, "\n", 1);
                }
            }
        }
    }

    /* marker */
    buf_t marker;
    buf_init(&marker);
    buf_append_str(&marker, "{\n  \"tool\": \"veritpath\",\n  \"payload\": \"");
    buf_append_str(&marker, p->name);
    buf_append_str(&marker, "\",\n  \"version\": \"");
    buf_append_str(&marker, p->version);
    buf_append_str(&marker, "\",\n  \"files\": [");
    for (int i = 0; i < p->n_files; i++) {
        if (i)
            buf_append_str(&marker, ", ");
        buf_appendf(&marker, "\"%s\"", p->files[i].dest);
    }
    buf_append_str(&marker, "]\n}\n");
    cpio_entry_t mk;
    memset(&mk, 0, sizeof(mk));
    mk.name = xstrdup("veritpath.json");
    mk.mode = 0100644;
    mk.data = marker;
    cpio_add(a, seg, &mk);
    free(mk.name);
    buf_free(&marker);
    result_add(r->added, &r->n_added, VP_MAX_FILES * 2, "/veritpath.json");
    return 0;
}
