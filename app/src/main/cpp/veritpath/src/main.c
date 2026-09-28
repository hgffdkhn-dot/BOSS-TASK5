/* veritpath - Android boot image analyzer and payload injector.
 *
 * _GNU_SOURCE is defined here rather than on the command line so the sources
 * compile identically under -std=c11, -std=gnu11 and any cross toolchain:
 * glibc otherwise hides PATH_MAX, strtok_r, symlink, readlink and lstat.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

/* veritpath command line interface. */
#include "vp.h"
uint32_t vp_detect_header_version(const uint8_t *d, size_t len, const char *path);

#include <getopt.h>
#include <stdlib.h>

static void usage(void)
{
    puts("veritpath - analyze boot/init_boot/vendor_boot images and inject "
         "third-party payloads");
    puts("");
    puts("usage: veritpath <command> [options]");
    puts("");
    puts("commands:");
    puts("  analyze   detect arch / layout and report the injection target");
    puts("  plan      print the injection plan (dry run)");
    puts("  inject    inject a payload and write a patched image");
    puts("  unpack    unpack an image into a directory");
    puts("  repack    rebuild an image from a directory");
    puts("  hexdump   dump the first 64 header bytes (diagnostics)");
    puts("  verify    check an injected image (exit 1 if incomplete)");
    puts("  payload-check  validate a payload directory");
    puts("  doctor    show which veritpath build is running");
    puts("");
    puts("input options:");
    puts("  --header-version N   force a header version (0-4, diagnostics)");
    puts("  --boot FILE          boot.img");
    puts("  --init-boot FILE     init_boot.img (Android 13+)");
    puts("  --vendor-boot FILE   vendor_boot.img");
    puts("  --recovery FILE      recovery.img");
    puts("");
    puts("inject/plan options:");
    puts("  -p, --payload DIR    payload directory");
    puts("  -o, --output PATH    output file or directory");
    puts("      --patch-vendor-boot   also patch the vendor ramdisk");
    puts("      --permissive     add androidboot.selinux=permissive");
    puts("      --cmdline TOKENS extra kernel cmdline tokens");
    puts("      --segment N      cpio segment index to patch");
    puts("      --format NAME    force ramdisk compression (gzip|lz4|lz4_legacy|xz|...)");
    puts("      --force          ignore compatibility checks and double injection");
    puts("      --no-backup      do not keep a backup of the original image");
    puts("      --dry-run        patch in memory only, write nothing");
    puts("      --json           machine readable output");
    puts("  -b, --brief          compact KEY:VALUE output (magiskboot style)");
    puts("      --header-version N  force a header version (0-4, diagnostics)");
    puts("");
    puts("examples:");
    puts("  veritpath analyze --boot boot.img --init-boot init_boot.img");
    puts("  veritpath plan --init-boot init_boot.img -p payloads/example-su");
    puts("  veritpath inject --init-boot init_boot.img -p payloads/example-su -o out/");
    puts("  veritpath unpack init_boot.img -d work/ && veritpath repack work -o new.img");
}

static const struct option kLongOpts[] = {
    {"boot", required_argument, 0, 'B'},
    {"init-boot", required_argument, 0, 'I'},
    {"vendor-boot", required_argument, 0, 'V'},
    {"recovery", required_argument, 0, 'R'},
    {"payload", required_argument, 0, 'p'},
    {"output", required_argument, 0, 'o'},
    {"dir", required_argument, 0, 'd'},
    {"patch-vendor-boot", no_argument, 0, 'P'},
    {"permissive", no_argument, 0, 'S'},
    {"cmdline", required_argument, 0, 'c'},
    {"segment", required_argument, 0, 'g'},
    {"format", required_argument, 0, 'F'},
    {"force", no_argument, 0, 'f'},
    {"no-backup", no_argument, 0, 'n'},
    {"dry-run", no_argument, 0, 'D'},
    {"json", no_argument, 0, 'j'},
    {"verbose", no_argument, 0, 'v'},
    {"brief", no_argument, 0, 'b'},
    {"help", no_argument, 0, 'h'},
    {"header-version", required_argument, 0, 'H'},
    {0, 0, 0, 0},
};

typedef struct {
    const char *boot, *init_boot, *vendor_boot, *recovery;
    const char *payload, *output, *dir;
    const char *positional;          /* unpack <image> / repack <dir> */
    options_t opts;
    int json;
    int brief;
} args_t;

static void parse_args(int argc, char **argv, args_t *a)
{
    memset(a, 0, sizeof(*a));
    options_init(&a->opts);
    optind = 1;
    for (;;) {
        int c = getopt_long(argc, argv, "p:o:d:B:I:V:R:c:g:F:fDnPjSvhH:b", kLongOpts, NULL);
        if (c == -1)
            break;
        switch (c) {
        case 'B': a->boot = optarg; break;
        case 'I': a->init_boot = optarg; break;
        case 'V': a->vendor_boot = optarg; break;
        case 'R': a->recovery = optarg; break;
        case 'p': a->payload = optarg; break;
        case 'o': a->output = optarg; break;
        case 'd': a->dir = optarg; break;
        case 'P': a->opts.patch_vendor_boot = 1; break;
        case 'S': a->opts.permissive = 1; break;
        case 'c': a->opts.cmdline = optarg; break;
        case 'g': a->opts.segment = atoi(optarg); break;
        case 'F': a->opts.ramdisk_format = comp_from_name(optarg); break;
        case 'f': a->opts.force = 1; break;
        case 'n': a->opts.no_backup = 1; break;
        case 'D': a->opts.dry_run = 1; break;
        case 'j': a->json = 1; break;
        case 'b': a->brief = 1; break;
        case 'v': vp_set_verbose(1); break;
        case 'H': vp_forced_header_version = atoi(optarg); break;
        case 'h': usage(); exit(0);
        default: break;
        }
    }
    if (optind < argc)
        a->positional = argv[optind];
}

static boot_img_t *g_imgs[VP_MAX_IMAGES];
static const char *g_roles[VP_MAX_IMAGES];
static size_t g_n;

static int load_images(args_t *a, image_set_t *set)
{
    struct {
        const char *role;
        const char *path;
    } spec[] = {
        {"boot", a->boot},
        {"init_boot", a->init_boot},
        {"vendor_boot", a->vendor_boot},
        {"recovery", a->recovery},
    };
    g_n = 0;
    for (size_t i = 0; i < 4; i++) {
        if (!spec[i].path)
            continue;
        buf_t data;
        buf_init(&data);
        if (read_file(spec[i].path, &data) != 0) {
            vp_report_missing(spec[i].role, spec[i].path);
            buf_free(&data);
            return -1;
        }
        boot_img_t *img = xmalloc(sizeof(boot_img_t));
        if (boot_img_parse(data.data, data.len, spec[i].role, spec[i].path, img) != 0) {
            boot_img_free(img);
            free(img);
            buf_free(&data);
            return -1;
        }
        img->path = spec[i].path;
        buf_free(&data);
        g_imgs[g_n] = img;
        g_roles[g_n] = spec[i].role;
        set[g_n].img = img;
        set[g_n].role = spec[i].role;
        g_n++;
    }
    if (g_n == 0) {
        vp_err("no input images given (use --boot/--init-boot/--vendor-boot)");
        return -1;
    }
    return 0;
}

static void free_images(void)
{
    for (size_t i = 0; i < g_n; i++) {
        boot_img_free(g_imgs[i]);
        free(g_imgs[i]);
    }
    g_n = 0;
}

static boot_img_t *img_by_role(const char *role)
{
    for (size_t i = 0; i < g_n; i++) {
        if (strcmp(g_roles[i], role) == 0)
            return g_imgs[i];
    }
    return NULL;
}

static void mkdir_p_for(const char *path)
{
    char *copy = xstrdup(path);
    char *slash = strrchr(copy, '/');
    if (slash) {
        *slash = 0;
        if (*copy)
            mkdir_p(copy);
    }
    free(copy);
}

static void backup_original(const char *src)
{
    buf_t data;
    buf_init(&data);
    if (read_file(src, &data) != 0) {
        buf_free(&data);
        return;
    }
    char *bak = xmalloc(strlen(src) + 32);
    sprintf(bak, "%s.veritpath.bak", src);
    if (write_file(bak, data.data, data.len) == 0)
        vp_log("backup of %s -> %s", src, bak);
    free(bak);
    buf_free(&data);
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static char *out_path_for(const char *src, const char *output, const char *role,
                          int multiple)
{
    const char *base = base_name(src);
    char name[256];
    if (multiple) {
        snprintf(name, sizeof(name), "%s.veritpath.img", role);
    } else {
        char *stemmed = replace_suffix(base, ".veritpath.img");
        snprintf(name, sizeof(name), "%s", stemmed);
        free(stemmed);
    }
    if (!output)
        return replace_suffix(src, ".veritpath.img");
    /* treat as a directory when it is one, or when it has no extension yet */
    int as_dir = is_dir(output) || (!file_exists(output) && !strchr(base_name(output), '.'));
    if (as_dir) {
        mkdir_p(output);
        return path_join(output, name);
    }
    if (multiple) {
        char *stem = replace_suffix(output, "");
        char *res = xmalloc(strlen(stem) + strlen(role) + 16);
        sprintf(res, "%s.%s.img", stem, role);
        free(stem);
        return res;
    }
    return xstrdup(output);
}

static int cmd_hexdump(args_t *a)
{
    const char *path = a->positional;
    if (!path) {
        vp_err("usage: veritpath hexdump <image>");
        return 1;
    }
    buf_t data;
    buf_init(&data);
    if (read_file(path, &data) != 0) {
        vp_report_missing("payload", path);
        buf_free(&data);
        return 1;
    }
    printf("FILE:%s\n", path);
    printf("SIZE:%zu\n", data.len);

    size_t n = data.len < 64 ? data.len : 64;
    char hex[64 * 3 + 1];
    char txt[65];
    size_t p = 0;
    for (size_t i = 0; i < n; i++) {
        p += (size_t)snprintf(hex + p, sizeof(hex) - p, "%02x ", data.data[i]);
        txt[i] = (data.data[i] >= 32 && data.data[i] < 127) ? (char)data.data[i] : '.';
    }
    txt[n] = 0;
    printf("HEAD64:%s\n", hex);
    printf("ASCII :%s\n", txt);

    char mhex[17];
    for (int i = 0; i < 8 && i < (int)n; i++)
        snprintf(mhex + i * 2, 3, "%02x", data.data[i]);
    if (n < 8)
        mhex[n * 2] = 0;
    char mascii[9];
    for (int i = 0; i < 8 && i < (int)n; i++)
        mascii[i] = (data.data[i] >= 32 && data.data[i] < 127) ? (char)data.data[i] : '.';
    mascii[n < 8 ? n : 8] = 0;
    printf("MAGIC8:%s  |%s|\n", mhex, mascii);

    puts("--- fields ---");
    static const struct {
        size_t off;
        const char *label;
    } f[] = {
        {8, "kernel_size"},
        {12, "ramdisk_size"},
        {16, "second_size|os_version"},
        {20, "header_size  (@20)"},
        {24, "header_ver   (@24)"},
        {36, "page_size    (@36)"},
        {40, "header_ver   (@40)"},
    };
    for (size_t i = 0; i < sizeof(f) / sizeof(f[0]); i++) {
        if (data.len >= f[i].off + 4) {
            const uint8_t *q = data.data + f[i].off;
            uint32_t v = (uint32_t)q[0] | ((uint32_t)q[1] << 8) |
                         ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);
            printf("  +%-3zu %-22s %u\n", f[i].off, f[i].label, v);
        } else {
            printf("  +%-3zu %-22s (eof)\n", f[i].off, f[i].label);
        }
    }

    puts("--- verdict ---");
    uint32_t hv = vp_detect_header_version(data.data, data.len, path);
    if (hv == 0xFFFFFFFFu)
        puts("detect_header_version:FAILED (no layout fits)");
    else
        printf("detect_header_version:%u\n", hv);

    buf_free(&data);
    return 0;
}

/* An image built by `veritpath inject`, checked without needing anything else.
 *
 * Reports what lands in the ramdisk, whether the rc hook and the file_contexts
 * label made it in, and whether the cmdline was patched. Exits non-zero if a
 * required piece is missing, so it can gate a flash in a script. */
static int cmd_verify(args_t *a)
{
    const char *path = a->positional;
    if (!path) {
        vp_err("usage: veritpath verify <image> [-p payload]");
        return 1;
    }
    buf_t data;
    buf_init(&data);
    if (read_file(path, &data) != 0) {
        vp_report_missing("image", path);
        buf_free(&data);
        return 1;
    }

    boot_img_t img;
    if (boot_img_parse(data.data, data.len, NULL, path, &img) != 0) {
        vp_err("%s: cannot parse", path);
        boot_img_free(&img);
        buf_free(&data);
        return 1;
    }

    int rc = 0;
    printf("FILE:%s\n", path);
    printf("HEADER_VER:%u\n", img.header_version);
    printf("PAGESIZE:%u\n", img.page_size);
    if (img.ramdisk.len) {
        printf("RAMDISK_SZ:%zu\n", img.ramdisk.len);
        printf("RAMDISK_FMT:%s\n",
               comp_name(comp_detect(img.ramdisk.data, img.ramdisk.len)));
    }

    cpio_archive_t arc;
    cpio_init(&arc);
    if (img.ramdisk.len && boot_img_ramdisk_archive(&img, &arc) == 0) {
        printf("SEGMENTS:%zu\n", arc.n);
        cpio_entry_t *marker = cpio_find(&arc, "veritpath.json");
        printf("PATCHED:%s\n", marker ? "1" : "0");
        if (!marker) {
            vp_err("  no veritpath marker in the ramdisk");
            rc = 1;
        } else if (vp_verbose) {
            fwrite(marker->data.data, 1, marker->data.len, stdout);
            putchar('\n');
        }

        /* files declared by a payload, when one is given */
        if (a->payload) {
            payload_t p;
            if (payload_load(a->payload, &p) == 0) {
                for (int i = 0; i < p.n_files; i++) {
                    cpio_entry_t *e = cpio_find(&arc, p.files[i].dest);
                    int ok = e != NULL;
                    if (ok && p.files[i].mode)
                        ok = CPIO_PERMS(e) == (p.files[i].mode & 07777);
                    printf("  %-28s %s\n", p.files[i].dest,
                           ok ? "present" : "MISSING");
                    if (!ok && p.files[i].required)
                        rc = 1;
                }
                if (p.rc.file[0]) {
                    cpio_entry_t *e = cpio_find(&arc, p.rc.file);
                    printf("  %-28s %s\n", p.rc.file, e ? "present" : "MISSING");
                    if (!e)
                        rc = 1;
                }
                for (int i = 0; i < p.rc.n_import; i++) {
                    cpio_entry_t *e = cpio_find(&arc, p.rc.import_into[i]);
                    int hooked = e && e->data.len &&
                                 memmem(e->data.data, e->data.len, "import ", 7);
                    printf("  %-28s %s\n", p.rc.import_into[i],
                           hooked ? "hooks the rc" : "NOT HOOKED");
                    if (!hooked)
                        rc = 1;
                }
                payload_free(&p);
            } else {
                vp_err("  cannot load payload %s", a->payload);
                rc = 1;
            }
        } else {
            /* no payload given: report what is obviously ours */
            static const char *known[] = {"/su", "/init.veritpath.rc",
                                          "/veritpath.json"};
            for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
                cpio_entry_t *e = cpio_find(&arc, known[i]);
                printf("  %-28s %s\n", known[i], e ? "present" : "absent");
            }
            cpio_entry_t *init = cpio_find(&arc, "/init.rc");
            int hooked = init && init->data.len &&
                         memmem(init->data.data, init->data.len, "import ", 7);
            printf("  %-28s %s\n", "/init.rc", hooked ? "hooks the rc" : "not hooked");
            cpio_entry_t *fc = cpio_find(&arc, "/file_contexts");
            int labelled = fc && fc->data.len &&
                           memmem(fc->data.data, fc->data.len, "u:object_r", 10);
            printf("  %-28s %s\n", "/file_contexts",
                   labelled ? "has labels" : "no labels");
        }
    } else {
        vp_err("  no ramdisk in this image");
        rc = 1;
    }
    cpio_free(&arc);

    const char *cl = boot_img_cmdline(&img);
    printf("CMDLINE:%s\n", cl && *cl ? cl : "(empty)");
    if (cl && strstr(cl, "androidboot.selinux=permissive"))
        puts("SELINUX:permissive");

    boot_img_free(&img);
    buf_free(&data);
    if (rc == 0)
        puts("VERDICT:OK");
    else
        puts("VERDICT:INCOMPLETE");
    return rc;
}

static int cmd_payload_check(args_t *a)
{
    const char *dir = a->positional;
    if (!dir) {
        vp_err("usage: veritpath payload-check <payload-dir> [arch] [api]");
        return 1;
    }
    payload_t p;
    if (payload_load(dir, &p) != 0) {
        vp_err("cannot load payload from %s", dir);
        return 1;
    }
    printf("NAME:%s\n", p.name);
    printf("FILES:%d\n", p.n_files);
    for (int i = 0; i < p.n_files; i++) {
        printf("  %-28s mode %04o%s%s\n", p.files[i].dest, p.files[i].mode & 07777,
               p.files[i].required ? "  required" : "",
               p.files[i].context[0] ? "  +selinux" : "");
    }
    if (p.rc.file[0]) {
        printf("RC:%s\n", p.rc.file);
        for (int i = 0; i < p.rc.n_import; i++)
            printf("  IMPORT_INTO:%s\n", p.rc.import_into[i]);
    }
    if (p.cmdline_append[0])
        printf("CMDLINE_APPEND:%s\n", p.cmdline_append);
    printf("SELINUX:%s\n", p.selinux[0] ? p.selinux : "(unchanged)");

    char msg[512];
    if (payload_check(&p, "arm64", 33, msg, sizeof(msg)) != 0 && msg[0]) {
        printf("WARN:%s\n", msg);
    }
    payload_free(&p);
    puts("VERDICT:OK");
    return 0;
}

static int cmd_analyze(args_t *a)
{
    image_set_t set[VP_MAX_IMAGES];
    if (load_images(a, set) != 0)
        return 1;
    analysis_t res;
    detect_analyze(set, g_n, &res);
    int mode = a->json ? VP_OUT_JSON : (a->brief ? VP_OUT_BRIEF : VP_OUT_RICH);
    detect_print(&res, mode);
    free_images();
    return 0;
}

static int cmd_plan(args_t *a)
{
    image_set_t set[VP_MAX_IMAGES];
    if (load_images(a, set) != 0)
        return 1;
    analysis_t res;
    detect_analyze(set, g_n, &res);
    payload_t p;
    int have_payload = 0;
    if (a->payload) {
        if (payload_load(a->payload, &p) != 0) {
            vp_err("cannot load payload from %s", a->payload);
            free_images();
            return 1;
        }
        have_payload = 1;
        char msg[512];
        if (payload_check(&p, res.arch, res.android_api, msg, sizeof(msg)) && !a->opts.force) {
            vp_err("payload incompatible: %s", msg);
            vp_err("pass --force to inject anyway");
            payload_free(&p);
            free_images();
            return 1;
        }
        if (msg[0])
            vp_warn("%s", msg);
    }
    strategy_print_plan(&res, have_payload ? &p : NULL, &a->opts, a->json);
    if (have_payload)
        payload_free(&p);
    free_images();
    return 0;
}

static int cmd_inject(args_t *a)
{
    image_set_t set[VP_MAX_IMAGES];
    if (load_images(a, set) != 0)
        return 1;
    analysis_t res;
    detect_analyze(set, g_n, &res);
    payload_t p;
    if (!a->payload) {
        vp_err("inject needs --payload DIR");
        free_images();
        return 1;
    }
    if (payload_load(a->payload, &p) != 0) {
        vp_err("cannot load payload from %s", a->payload);
        free_images();
        return 1;
    }
    char msg[512];
    if (payload_check(&p, res.arch, res.android_api, msg, sizeof(msg)) && !a->opts.force) {
        vp_err("payload incompatible: %s", msg);
        vp_err("pass --force to inject anyway");
        payload_free(&p);
        free_images();
        return 1;
    }
    if (msg[0])
        vp_warn("%s", msg);

    strategy_print_plan(&res, &p, &a->opts, a->json);
    putchar('\n');

    if (!res.target[0]) {
        vp_err("no ramdisk found in any supplied image - nothing to inject");
        payload_free(&p);
        free_images();
        return 1;
    }
    if (res.already_patched && !a->opts.force) {
        vp_err("image already carries a veritpath payload; use --force to re-inject");
        payload_free(&p);
        free_images();
        return 1;
    }

    /* which images get patched */
    const char *targets[VP_MAX_IMAGES];
    size_t ntargets = 0;
    targets[ntargets++] = res.target;
    if (a->opts.patch_vendor_boot && strcmp(res.target, "vendor_boot") != 0 &&
        img_by_role("vendor_boot"))
        targets[ntargets++] = "vendor_boot";

    for (size_t t = 0; t < ntargets; t++) {
        boot_img_t *img = img_by_role(targets[t]);
        if (!img)
            continue;
        cpio_archive_t arc;
        cpio_init(&arc);
        if (boot_img_ramdisk_archive(img, &arc) != 0) {
            vp_err("%s: cannot read ramdisk", targets[t]);
            cpio_free(&arc);
            continue;
        }
        size_t seg = a->opts.segment >= 0 ? (size_t)a->opts.segment : cpio_main_segment(&arc);
        if (seg >= arc.n)
            seg = arc.n - 1;

        inject_result_t r;
        memset(&r, 0, sizeof(r));
        /* vendor_boot: every fragment needs the payload */
        if (img->is_vendor && arc.n > 1) {
            for (size_t s = 0; s < arc.n; s++)
                payload_apply(&arc, s, &p, &r, a->opts.permissive, a->opts.cmdline);
        } else {
            payload_apply(&arc, seg, &p, &r, a->opts.permissive, a->opts.cmdline);
        }
        boot_img_set_ramdisk(img, &arc, a->opts.ramdisk_format);

        if (t == 0) {
            if (a->opts.permissive)
                boot_img_append_cmdline(img, "androidboot.selinux=permissive");
            if (a->opts.cmdline)
                boot_img_append_cmdline(img, a->opts.cmdline);
            if (p.cmdline_append[0])
                boot_img_append_cmdline(img, p.cmdline_append);
        }

        if (a->opts.dry_run) {
            vp_log("[%s] dry run - nothing written", targets[t]);
            cpio_free(&arc);
            continue;
        }

        buf_t packed;
        buf_init(&packed);
        boot_img_pack(img, &packed);
        char *out = out_path_for(img->path, a->output, targets[t], ntargets > 1);
        if (!a->opts.no_backup)
            backup_original(img->path);
        mkdir_p_for(out);
        if (write_file(out, packed.data, packed.len) != 0) {
            vp_err("cannot write %s", out);
        } else {
            vp_log("wrote %s (%s)", out, human_size(packed.len));
            printf("  [%s] -> %s\n", targets[t], out);
            for (int i = 0; i < r.n_added; i++)
                printf("    added    %s\n", r.added[i]);
            for (int i = 0; i < r.n_replaced; i++)
                printf("    replaced %s\n", r.replaced[i]);
            for (int i = 0; i < r.n_backed; i++)
                printf("    kept     %s\n", r.backed_up[i]);
        }
        buf_free(&packed);
        free(out);
        cpio_free(&arc);
    }
    payload_free(&p);
    free_images();
    return 0;
}

static int cmd_unpack(args_t *a)
{
    if (!a->dir) {
        vp_err("unpack needs -d DIR");
        return 1;
    }
    const char *img_path = a->positional ? a->positional
                                         : (a->boot ? a->boot : a->init_boot);
    if (!img_path) {
        vp_err("usage: veritpath unpack <image> -d <dir>");
        return 1;
    }
    buf_t data;
    buf_init(&data);
    if (read_file(img_path, &data) != 0) {
        vp_report_missing("image", img_path);
        return 1;
    }
    boot_img_t img;
    if (boot_img_parse(data.data, data.len, NULL, img_path, &img) != 0) {
        boot_img_free(&img);
        buf_free(&data);
        return 1;
    }
    mkdir_p(a->dir);
    /* keep the original so repack can restore header/kernel/cmdline */
    char *orig = path_join(a->dir, "original.img");
    write_file(orig, data.data, data.len);
    free(orig);

    if (img.kernel.len) {
        char *p = path_join(a->dir, "kernel");
        write_file(p, img.kernel.data, img.kernel.len);
        free(p);
    }
    if (img.dtb.len) {
        char *p = path_join(a->dir, "dtb");
        write_file(p, img.dtb.data, img.dtb.len);
        free(p);
    }
    if (img.bootconfig.len) {
        char *p = path_join(a->dir, "bootconfig");
        write_file(p, img.bootconfig.data, img.bootconfig.len);
        free(p);
    }
    if (img.ramdisk.len) {
        cpio_archive_t arc;
        cpio_init(&arc);
        if (boot_img_ramdisk_archive(&img, &arc) == 0) {
            char *rd = path_join(a->dir, "ramdisk");
            mkdir_p(rd);
            cpio_extract_dir(&arc, rd);
            char *cp = path_join(a->dir, "ramdisk.cpio");
            buf_t raw;
            buf_init(&raw);
            cpio_serialize(&arc, &raw);
            write_file(cp, raw.data, raw.len);
            buf_free(&raw);
            free(cp);
            free(rd);
            vp_log("unpacked %s into %s (%zu entries)", img_path, a->dir,
                   arc.segs[0].n);
            cpio_free(&arc);
        }
    }
    printf("  kernel    %s\n", human_size(img.kernel.len));
    printf("  ramdisk   %s\n", human_size(img.ramdisk.len));
    printf("  dtb       %s\n", human_size(img.dtb.len));
    boot_img_free(&img);
    buf_free(&data);
    return 0;
}

static int cmd_repack(args_t *a)
{
    const char *dir = a->positional ? a->positional : a->dir;
    if (!dir || !a->output) {
        vp_err("usage: veritpath repack <dir> -o <output>");
        return 1;
    }
    a->dir = dir;
    char *orig = path_join(a->dir, "original.img");
    buf_t data;
    buf_init(&data);
    if (read_file(orig, &data) != 0) {
        vp_err("%s missing - only directories created by 'veritpath unpack' can be "
               "repacked",
               orig);
        free(orig);
        return 1;
    }
    boot_img_t img;
    /* orig is stored as img.path, so it must outlive the parsed image */
    if (boot_img_parse(data.data, data.len, NULL, orig, &img) != 0) {
        free(orig);
        buf_free(&data);
        return 1;
    }
    char *rd = path_join(a->dir, "ramdisk");
    if (is_dir(rd)) {
        cpio_archive_t arc;
        cpio_init(&arc);
        cpio_build_dir(rd, &arc);
        boot_img_set_ramdisk(&img, &arc, -1);
        vp_log("rebuilt ramdisk from %s", rd);
        cpio_free(&arc);
    }
    free(rd);
    char *kp = path_join(a->dir, "kernel");
    if (file_exists(kp) && !img.is_vendor) {
        buf_t k;
        buf_init(&k);
        if (read_file(kp, &k) == 0) {
            buf_reset(&img.kernel);
            buf_append(&img.kernel, k.data, k.len);
        }
        buf_free(&k);
    }
    free(kp);

    buf_t packed;
    buf_init(&packed);
    boot_img_pack(&img, &packed);
    mkdir_p_for(a->output);
    if (write_file(a->output, packed.data, packed.len) != 0) {
        vp_err("cannot write %s", a->output);
        buf_free(&packed);
        boot_img_free(&img);
        free(orig);
        buf_free(&data);
        return 1;
    }
    vp_log("wrote %s (%s)", a->output, human_size(packed.len));
    buf_free(&packed);
    boot_img_free(&img);
    free(orig);
    buf_free(&data);
    return 0;
}

/* argv[0] is the sub-command (no program name) so embedders can pass exactly
 * what a user would type after "veritpath". */
static const char *g_program_name = "veritpath";

void vp_set_program_name(const char *name)
{
    if (name && *name)
        g_program_name = name;
}

int vp_cli_run(int argc, char **argv)
{
    if (argc < 1 || !argv || !argv[0]) {
        usage();
        return 1;
    }
    const char *cmd = argv[0];
    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0 ||
        strcmp(cmd, "help") == 0) {
        usage();
        return 0;
    }
    if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "version") == 0) {
        printf("veritpath %s\n", VP_VERSION);
        return 0;
    }
    args_t a;
    /* parse_args() skips argv[0] the way getopt expects a program name, and
     * here argv[0] is the sub-command - so hand it the array as-is. */
    parse_args(argc, argv, &a);
    if (strcmp(cmd, "hexdump") == 0)
        return cmd_hexdump(&a);
    if (strcmp(cmd, "doctor") == 0) {
        printf("VERSION:%s\n", VP_VERSION);
        printf("BINARY:%s\n", g_program_name);
        /* A bare `veritpath` only resolves when its directory is on PATH.
         * On many Android shells it is not, and users then hit
         * "inaccessible or not found" even though the binary is right there. */
        const char *path = getenv("PATH");
        int on_path = 0;
        if (path && g_program_name) {
            if (!strchr(g_program_name, '/')) {
                /* invoked as a bare name: the shell already resolved it,
                 * so it must have come from PATH */
                on_path = 1;
            } else {
                char *dir = path_dirname(g_program_name);
                if (dir) {
                    on_path = dir_in_path(dir, path);
                    free(dir);
                }
            }
        }
        printf("ON_PATH:%s\n", on_path ? "1" : "0");
        if (!on_path)
            printf("HINT:this directory is not in PATH - run ./veritpath or the "
                   "full path\n");
        printf("VERDICT:OK\n");
        return 0;
    }
    if (strcmp(cmd, "verify") == 0)
        return cmd_verify(&a);
    if (strcmp(cmd, "payload-check") == 0)
        return cmd_payload_check(&a);
    if (strcmp(cmd, "analyze") == 0)
        return cmd_analyze(&a);
    if (strcmp(cmd, "plan") == 0)
        return cmd_plan(&a);
    if (strcmp(cmd, "inject") == 0)
        return cmd_inject(&a);
    if (strcmp(cmd, "unpack") == 0)
        return cmd_unpack(&a);
    if (strcmp(cmd, "repack") == 0)
        return cmd_repack(&a);
    vp_err("unknown command: %s", cmd);
    usage();
    return 1;
}

/* Library builds (the shared object and the JNI binding) define VP_NO_MAIN so
 * they do not carry a second main(). */
#ifndef VP_NO_MAIN
int main(int argc, char **argv)
{
    if (argc > 0)
        vp_set_program_name(argv[0]);
    return vp_cli_run(argc - 1, argv + 1);
}
#endif
