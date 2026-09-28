/* veritpath - Android boot image analyzer and payload injector.
 *
 * _GNU_SOURCE is defined here rather than on the command line so the sources
 * compile identically under -std=c11, -std=gnu11 and any cross toolchain:
 * glibc otherwise hides PATH_MAX, strtok_r, symlink, readlink and lstat.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

/* Architecture / layout detection. */
#include "vp.h"

#include <stdlib.h>

static const struct {
    int api;
    const char *ver;
} api_table[] = {
    {26, "8.0"}, {27, "8.1"}, {28, "9"}, {29, "10"}, {30, "11"},
    {31, "12"}, {32, "12L"}, {33, "13"}, {34, "14"}, {35, "15"}, {36, "16"},
};

static uint32_t rd32(const uint8_t *p, size_t off)
{
    return (uint32_t)(p[off] | (p[off + 1] << 8) | (p[off + 2] << 16) |
                      ((uint32_t)p[off + 3] << 24));
}

static void decode_os_version(uint32_t v, char *out, size_t outsz)
{
    if (!v) {
        snprintf(out, outsz, "unknown");
        return;
    }
    uint32_t ver = v >> 11;
    uint32_t patch = v & 0x7FF;
    uint32_t a = (ver >> 14) & 0x3F, b = (ver >> 7) & 0x7F, c = ver & 0x7F;
    if (ver) {
        snprintf(out, outsz, "%u.%u.%u", a, b, c);
        return;
    }
    (void)patch;
    snprintf(out, outsz, "unknown");
}

int detect_arch(const uint8_t *kernel, size_t klen, const uint8_t *dtb, size_t dlen,
                char *out, size_t outsz)
{
    snprintf(out, outsz, "unknown");
    if (kernel && klen > 0) {
        buf_t raw;
        buf_init(&raw);
        if (comp_decompress(kernel, klen, &raw) == 0 && raw.len) {
            if (raw.len > 0x40 && raw.data[0] == 'M' && raw.data[1] == 'Z' &&
                vp_memmem(raw.data, 0x60, "ARM64", 5) != NULL) {
                snprintf(out, outsz, "arm64");
                buf_free(&raw);
                return 0;
            }
            if (raw.len > 20 && raw.data[0] == 0x7f && memcmp(raw.data + 1, "ELF", 3) == 0) {
                static const struct {
                    uint16_t m;
                    const char *n;
                } machines[] = {
                    {3, "x86"}, {40, "arm"}, {62, "x86_64"}, {183, "arm64"},
                    {243, "riscv64"}, {258, "riscv64"},
                };
                uint16_t m = (uint16_t)(raw.data[18] | (raw.data[19] << 8));
                for (size_t i = 0; i < sizeof(machines) / sizeof(machines[0]); i++) {
                    if (machines[i].m == m) {
                        snprintf(out, outsz, "%s", machines[i].n);
                        buf_free(&raw);
                        return 0;
                    }
                }
            }
            if (raw.len > 0x28 && rd32(raw.data, 0x24) == 0x016F2818u) {
                snprintf(out, outsz, "arm");
                buf_free(&raw);
                return 0;
            }
            if (raw.len > 0x206 && memcmp(raw.data + 0x202, "HdrS", 4) == 0) {
                uint16_t xload = raw.len > 0x238
                                     ? (uint16_t)(raw.data[0x236] | (raw.data[0x237] << 8))
                                     : 0;
                snprintf(out, outsz, "%s", (xload & 1) ? "x86_64" : "x86");
                buf_free(&raw);
                return 0;
            }
        }
        buf_free(&raw);
    }
    if (dtb && dlen) {
        if (vp_memmem(dtb, dlen, "arm,armv8", 9) || vp_memmem(dtb, dlen, "arm,arm-v8", 10)) {
            snprintf(out, outsz, "arm64");
            return 0;
        }
        if (vp_memmem(dtb, dlen, "arm,v7", 6) || vp_memmem(dtb, dlen, "arm,arm1176", 11)) {
            snprintf(out, outsz, "arm");
            return 0;
        }
        if (vp_memmem(dtb, dlen, "riscv", 5)) {
            snprintf(out, outsz, "riscv64");
            return 0;
        }
    }
    return 0;
}

static const boot_img_t *find_img(image_set_t *set, size_t n, const char *role)
{
    for (size_t i = 0; i < n; i++) {
        if (set[i].img && strcmp(set[i].role, role) == 0)
            return set[i].img;
    }
    return NULL;
}

static void guess_api(image_set_t *set, size_t n, int *api, char *ver, size_t verz)
{
    const boot_img_t *boot = find_img(set, n, "boot");
    const boot_img_t *init_boot = find_img(set, n, "init_boot");
    const boot_img_t *vendor = find_img(set, n, "vendor_boot");
    const boot_img_t *ref = boot ? boot : init_boot;
    if (ref && ref->os_version) {
        char v[32];
        decode_os_version(ref->os_version, v, sizeof(v));
        if (strcmp(v, "unknown") != 0) {
            int major = atoi(v);
            for (size_t i = 0; i < sizeof(api_table) / sizeof(api_table[0]); i++) {
                if (atoi(api_table[i].ver) == major) {
                    *api = api_table[i].api;
                    snprintf(ver, verz, "%s", api_table[i].ver);
                    return;
                }
            }
            snprintf(ver, verz, "%.15s", v);
            *api = 0;
            return;
        }
    }
    if (init_boot) {
        *api = 33;
        snprintf(ver, verz, "13");
        return;
    }
    if (vendor && vendor->header_version >= 4) {
        *api = 31;
        snprintf(ver, verz, "12");
        return;
    }
    uint32_t hv = ref ? ref->header_version : 0;
    switch (hv) {
    case 4: *api = 31; snprintf(ver, verz, "12"); break;
    case 3: *api = 30; snprintf(ver, verz, "11"); break;
    case 2: *api = 29; snprintf(ver, verz, "10"); break;
    case 1: *api = 27; snprintf(ver, verz, "8.1"); break;
    default: *api = 25; snprintf(ver, verz, "7.1"); break;
    }
}

static int looks_system_as_root(const boot_img_t *img, int api)
{
    const char *cl = boot_img_cmdline((boot_img_t *)img);
    if (strstr(cl, "system_as_root"))
        return 1;
    cpio_archive_t a;
    cpio_init(&a);
    if (boot_img_ramdisk_archive((boot_img_t *)img, &a) == 0) {
        int has_system_init = cpio_find(&a, "system/bin/init") != NULL;
        int has_init = cpio_find(&a, "init") != NULL;
        int has_system_dir = cpio_find(&a, "system") != NULL;
        cpio_free(&a);
        if (has_system_init)
            return 0;
        if (has_init && has_system_dir)
            return 1;
        if (has_init)
            return 1;
    } else {
        cpio_free(&a);
    }
    return api >= 28;
}

void detect_analyze(image_set_t *set, size_t n, analysis_t *res)
{
    memset(res, 0, sizeof(*res));
    snprintf(res->arch, sizeof(res->arch), "unknown");
    snprintf(res->android_version, sizeof(res->android_version), "unknown");
    snprintf(res->layout, sizeof(res->layout), "unknown");
    res->android_api = 0;

    const boot_img_t *boot = find_img(set, n, "boot");
    const boot_img_t *init_boot = find_img(set, n, "init_boot");
    const boot_img_t *vendor = find_img(set, n, "vendor_boot");
    const boot_img_t *recovery = find_img(set, n, "recovery");
    res->boot = boot;
    res->init_boot = init_boot;
    res->vendor_boot = vendor;
    res->has_vendor_boot = vendor != NULL;

    /* arch: prefer an image that carries a kernel */
    const boot_img_t *ksrc = boot;
    if (!ksrc || !ksrc->kernel.len)
        ksrc = recovery;
    if (!ksrc || !ksrc->kernel.len)
        ksrc = init_boot;
    if (ksrc && ksrc->kernel.len) {
        buf_t *dtb = boot_img_dtb((boot_img_t *)ksrc);
        detect_arch(ksrc->kernel.data, ksrc->kernel.len, dtb ? dtb->data : NULL,
                    dtb ? dtb->len : 0, res->arch, sizeof(res->arch));
    } else if (vendor && vendor->dtb.len) {
        detect_arch(NULL, 0, vendor->dtb.data, vendor->dtb.len, res->arch,
                    sizeof(res->arch));
    }

    guess_api(set, n, &res->android_api, res->android_version,
              sizeof(res->android_version));

    /* where is the ramdisk */
    const boot_img_t *target = NULL;
    if (init_boot && init_boot->ramdisk.len) {
        target = init_boot;
        snprintf(res->layout, sizeof(res->layout), "init_boot");
        snprintf(res->target, sizeof(res->target), "init_boot");
    } else if (boot && boot->ramdisk.len) {
        target = boot;
        snprintf(res->layout, sizeof(res->layout), "boot");
        snprintf(res->target, sizeof(res->target), "boot");
    } else if (recovery && recovery->ramdisk.len) {
        target = recovery;
        snprintf(res->layout, sizeof(res->layout), "boot");
        snprintf(res->target, sizeof(res->target), "recovery");
    } else if (vendor && vendor->ramdisk.len) {
        target = vendor;
        snprintf(res->layout, sizeof(res->layout), "vendor_boot");
        snprintf(res->target, sizeof(res->target), "vendor_boot");
    }
    res->target_img = target;

    res->gki = (init_boot != NULL) ||
               (boot && boot->header_version >= 3 && !boot->ramdisk.len);
    if (vendor) {
        for (size_t i = 0; i < vendor->n_frags; i++) {
            if (vendor->frags[i].type == 2)
                res->recovery_fragment = 1;
        }
    }

    if (target) {
        res->system_as_root = looks_system_as_root(target, res->android_api);
        cpio_archive_t a;
        cpio_init(&a);
        if (boot_img_ramdisk_archive((boot_img_t *)target, &a) == 0) {
            res->n_segments = (int)a.n;
            res->already_patched = cpio_find(&a, "veritpath.json") != NULL ||
                                   cpio_find(&a, "init.veritpath.rc") != NULL;
            cpio_free(&a);
        }
    }

    /* A/B slot */
    const char *cl = target ? boot_img_cmdline((boot_img_t *)target) : "";
    const char *slot = strstr(cl, "androidboot.slot_suffix=");
    if (slot) {
        slot += strlen("androidboot.slot_suffix=");
        if (*slot == '_')
            slot++;
        snprintf(res->slot, sizeof(res->slot), "%c", *slot);
    }
}

static const char *layout_advice(const char *layout)
{
    if (strcmp(layout, "init_boot") == 0)
        return "Android 13+ GKI: the generic ramdisk lives in init_boot.img, "
               "boot.img only carries the kernel. Patch init_boot.img.";
    if (strcmp(layout, "vendor_boot") == 0)
        return "GKI 1.0 (Android 11/12): boot.img has no ramdisk, the generic "
               "ramdisk is the vendor_ramdisk inside vendor_boot.img. Patch it "
               "there and keep the fragment table in sync.";
    if (strcmp(layout, "boot") == 0)
        return "Ramdisk lives inside this image. Patch it directly; kernel and "
               "DTB stay untouched.";
    return "No ramdisk found. Supply init_boot.img (Android 13+) or a boot.img "
           "that actually contains one.";
}

static void print_img(const char *path, const boot_img_t *img)
{
    if (!img)
        return;
    printf("[%s]\n", path ? path : img->role);
    printf("HEADER_VER:%u\n", img->header_version);
    if (!img->is_vendor) {
        if (img->kernel.len) {
            printf("KERNEL_SZ:%zu\n", img->kernel.len);
            printf("KERNEL_FMT:%s\n",
                   comp_name(comp_detect(img->kernel.data, img->kernel.len)));
        }
        if (img->second.len)
            printf("SECOND_SZ:%zu\n", img->second.len);
        if (img->recovery_dtbo.len)
            printf("RECOV_DTBO_SZ:%zu\n", img->recovery_dtbo.len);
    }
    if (img->ramdisk.len) {
        printf("RAMDISK_SZ:%zu\n", img->ramdisk.len);
        printf("RAMDISK_FMT:%s\n",
               comp_name(comp_detect(img->ramdisk.data, img->ramdisk.len)));
    }
    if (img->dtb.len)
        printf("DTB_SZ:%zu\n", img->dtb.len);
    printf("PAGESIZE:%u\n", img->page_size);
    const char *cl = boot_img_cmdline((boot_img_t *)img);
    if (cl) {
        while (*cl == ' ')
            cl++;
        if (*cl)
            printf("CMDLINE:%s\n", cl);
    }
    if (img->n_frags) {
        printf("FRAGMENTS:");
        for (size_t i = 0; i < img->n_frags; i++)
            printf("%s%s", i ? "," : "", img->frags[i].name);
        putchar('\n');
    }
    putchar('\n');
}

/* not called yn(): that collides with the Bessel function builtin */
static const char *yesno(int v)
{
    return v ? "1" : "0";
}

/* ------------------------------------------------------------ rich report */

typedef struct {
    int entries;
    int has_init;
    int contains_system_init;
    char segments[256];
} ramdisk_info_t;

static void ramdisk_info(const boot_img_t *img, ramdisk_info_t *out)
{
    memset(out, 0, sizeof(*out));
    snprintf(out->segments, sizeof(out->segments), "[]");
    if (!img)
        return;

    cpio_archive_t a;
    cpio_init(&a);
    if (boot_img_ramdisk_archive((boot_img_t *)img, &a) != 0) {
        cpio_free(&a);
        return;
    }
    size_t total = 0;
    for (size_t i = 0; i < a.n; i++)
        total += a.segs[i].n;
    out->entries = (int)total;
    out->has_init = cpio_find(&a, "init") != NULL;
    out->contains_system_init = cpio_find(&a, "system/bin/init") != NULL;

    char buf[256];
    size_t pos = 0;
    pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "[");
    for (size_t i = 0; i < a.n; i++) {
        const char *label = a.segs[i].label;
        if (!label || !*label)
            label = "segment";
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "%s'%s'",
                                i ? ", " : "", label);
        if (pos >= sizeof(buf))
            break;
    }
    snprintf(buf + pos, sizeof(buf) - pos, "]");
    snprintf(out->segments, sizeof(out->segments), "%s", buf);
    cpio_free(&a);
}

static void print_rule(char c)
{
    for (int i = 0; i < 62; i++)
        putchar(c);
    putchar('\n');
}

static void kv(const char *key, const char *val)
{
    printf("  %-20s %s\n", key, val);
}

static void kv_note(const char *key, const char *val, const char *note)
{
    printf("  %-20s %s  (%s)\n", key, val, note);
}

static void kv_bool(const char *key, int v)
{
    printf("  %-20s %s\n", key, v ? "True" : "False");
}

static void kv_bool_note(const char *key, int v, const char *note)
{
    printf("  %-20s %s  (%s)\n", key, v ? "True" : "False", note);
}

static void kv_int(const char *key, long v)
{
    printf("  %-20s %ld\n", key, v);
}

/* os_version: bits 11+ are a.b.c, the low 11 bits are the patch level */
static void os_version_str(uint32_t v, char *out, size_t outsz)
{
    if (!v) {
        snprintf(out, outsz, "unknown");
        return;
    }
    uint32_t ver = v >> 11;
    uint32_t a = (ver >> 14) & 0x3F, b = (ver >> 7) & 0x7F, c = ver & 0x7F;
    snprintf(out, outsz, "%u.%u.%u", a, b, c);
}

static void patch_level_str(uint32_t v, char *out, size_t outsz)
{
    uint32_t patch = v & 0x7FF;
    if (!patch) {
        snprintf(out, outsz, "unknown");
        return;
    }
    snprintf(out, outsz, "%04u-%02u", 2000 + (patch >> 4), patch & 0xF);
}

/* one "role.field  value  (note)" block per supplied image */
static void print_img_rich(const char *role, const boot_img_t *img)
{
    if (!img)
        return;
    char key[64];

    snprintf(key, sizeof(key), "%s.header_version", role);
    printf("  %-20s %u  (%s image header)\n", key, img->header_version,
           img->is_vendor ? "vendor boot" : "boot");
    if (!img->is_vendor && img->kernel.len) {
        snprintf(key, sizeof(key), "%s.kernel_size", role);
        kv_int(key, (long)img->kernel.len);
    }
    if (img->ramdisk.len) {
        snprintf(key, sizeof(key), "%s.ramdisk_size", role);
        kv_int(key, (long)img->ramdisk.len);
        snprintf(key, sizeof(key), "%s.ramdisk_format", role);
        kv(key, comp_name(comp_detect(img->ramdisk.data, img->ramdisk.len)));
    }
    snprintf(key, sizeof(key), "%s.page_size", role);
    printf("  %-20s %u\n", key, img->page_size);

    const char *cl = boot_img_cmdline((boot_img_t *)img);
    if (cl) {
        while (*cl == ' ')
            cl++;
        if (*cl) {
            snprintf(key, sizeof(key), "%s.cmdline", role);
            kv(key, cl);
        }
    }
    if (img->os_version) {
        char v[32], p[32];
        os_version_str(img->os_version, v, sizeof(v));
        patch_level_str(img->os_version, p, sizeof(p));
        snprintf(key, sizeof(key), "%s.os_version", role);
        char note[64];
        snprintf(note, sizeof(note), "patch level %s", p);
        printf("  %-20s %s  (%s)\n", key, v, note);
    }
    if (img->n_frags) {
        snprintf(key, sizeof(key), "%s.fragments", role);
        char names[256];
        size_t pos = 0;
        pos += (size_t)snprintf(names + pos, sizeof(names) - pos, "[");
        for (size_t i = 0; i < img->n_frags; i++)
            pos += (size_t)snprintf(names + pos, sizeof(names) - pos,
                                    "%s'%s'", i ? ", " : "", img->frags[i].name);
        snprintf(names + pos, sizeof(names) - pos, "]");
        kv(key, names);
    }
}

static void print_rich(analysis_t *res)
{
    printf("veritpath %s \xe2\x80\x94 boot image analysis\n", VP_VERSION);
    print_rule('=');

    kv("arch", res->arch);
    kv("android_version", res->android_version);
    kv("ramdisk_layout", res->layout);
    kv_bool("system_as_root", res->system_as_root);
    kv_bool("gki", res->gki);

    const boot_img_t *ksrc = res->boot;
    if (!ksrc || !ksrc->kernel.len)
        ksrc = res->init_boot;
    if (!ksrc || !ksrc->kernel.len)
        ksrc = res->vendor_boot;
    if (ksrc && ksrc->kernel.len)
        kv("kernel_compression",
           comp_name(comp_detect(ksrc->kernel.data, ksrc->kernel.len)));

    ramdisk_info_t ri;
    ramdisk_info(res->target_img, &ri);
    if (res->target_img && res->target_img->ramdisk.len)
        kv("ramdisk_compression",
           comp_name(comp_detect(res->target_img->ramdisk.data,
                                 res->target_img->ramdisk.len)));
    kv("ramdisk_segments", ri.segments);
    kv_bool("already_patched", res->already_patched);

    if (res->boot || res->init_boot || res->vendor_boot) {
        print_rule('-');
        print_img_rich("boot", res->boot);
        print_img_rich("init_boot", res->init_boot);
        print_img_rich("vendor_boot", res->vendor_boot);
    }

    if (res->target_img) {
        if (ksrc && ksrc->kernel.len)
            kv_note("kernel_compression",
                    comp_name(comp_detect(ksrc->kernel.data, ksrc->kernel.len)),
                    "kernel payload");
        kv_note("ramdisk_segments", ri.segments, "cpio segments inside the ramdisk");
        kv_int("ramdisk_entries", ri.entries);
        kv_bool_note("has_init", ri.has_init, "/init present in ramdisk");
        printf("  %-20s %s  (/system/bin/init inside the ramdisk)\n",
               "ramdisk_contains_system", ri.contains_system_init ? "True" : "False");
        kv_bool_note("already_patched", res->already_patched,
                     "no veritpath marker in ramdisk");
        if (res->target_img->ramdisk.len)
            kv_note("ramdisk_compression",
                    comp_name(comp_detect(res->target_img->ramdisk.data,
                                          res->target_img->ramdisk.len)),
                    "ramdisk payload magic");
    }

    print_rule('-');
    printf("  \xc2\xb7 %s\n", layout_advice(res->layout));
    if (res->system_as_root)
        puts("  \xc2\xb7 system-as-root: the ramdisk is only the first-stage init, "
             "/system is mounted as '/'. Inject into the ramdisk, not into /system.");
    else
        puts("  \xc2\xb7 legacy root layout: injected files stay visible in / after boot.");
    if (ri.entries && res->n_segments > 1)
        puts("  \xc2\xb7 Multi-stage ramdisk: the first stage is loaded before the "
             "main one - init/overlay files must go into the main segment.");
    if (res->target_img && res->target_img->ramdisk.len) {
        char line[256];
        snprintf(line, sizeof(line),
                 "  \xc2\xb7 Ramdisk is %s compressed \xe2\x80\x94 veritpath rebuilds "
                 "it with the same format.",
                 comp_name(comp_detect(res->target_img->ramdisk.data,
                                       res->target_img->ramdisk.len)));
        puts(line);
    }
    if (res->gki)
        puts("  \xc2\xb7 GKI device: the kernel is never touched, only the ramdisk.");
    if (res->recovery_fragment)
        puts("  \xc2\xb7 vendor_boot holds a RECOVERY fragment: add "
             "--patch-vendor-boot to reach recovery/fastbootd.");
    if (res->already_patched)
        puts("  \xc2\xb7 image already carries a veritpath payload "
             "(--force to re-inject)");
    if (strcmp(res->arch, "unknown") == 0)
        puts("  \xc2\xb7 arch unknown: init_boot.img has no kernel, supply boot.img too");
    if (res->slot[0])
        printf("  \xc2\xb7 A/B slot: %s\n", res->slot);

    print_rule('-');
    kv("injection target", res->target[0] ? res->target : "none");
}

void detect_print(analysis_t *res, int mode)
{
    if (mode == VP_OUT_JSON) {
        printf("{\n");
        printf("  \"arch\": \"%s\",\n", res->arch);
        printf("  \"android_api\": %d,\n", res->android_api);
        printf("  \"android_version\": \"%s\",\n", res->android_version);
        printf("  \"ramdisk_layout\": \"%s\",\n", res->layout);
        printf("  \"target\": \"%s\",\n", res->target);
        printf("  \"system_as_root\": %s,\n", res->system_as_root ? "true" : "false");
        printf("  \"gki\": %s,\n", res->gki ? "true" : "false");
        printf("  \"already_patched\": %s,\n", res->already_patched ? "true" : "false");
        printf("  \"ramdisk_segments\": %d\n", res->n_segments);
        printf("}\n");
        return;
    }

    if (mode == VP_OUT_BRIEF) {
        print_img(res->boot ? res->boot->path : NULL, res->boot);
        print_img(res->init_boot ? res->init_boot->path : NULL, res->init_boot);
        print_img(res->vendor_boot ? res->vendor_boot->path : NULL, res->vendor_boot);
        printf("ARCH:%s\n", res->arch);
        if (res->android_api)
            printf("ANDROID:%s (API %d)\n", res->android_version, res->android_api);
        else
            printf("ANDROID:%s\n", res->android_version);
        printf("LAYOUT:%s\n", res->layout);
        printf("SYSTEM_AS_ROOT:%s\n", yesno(res->system_as_root));
        printf("GKI:%s\n", yesno(res->gki));
        printf("SEGMENTS:%d\n", res->n_segments);
        printf("PATCHED:%s\n", yesno(res->already_patched));
        printf("TARGET:%s\n", res->target[0] ? res->target : "none");
        if (res->has_vendor_boot && strcmp(res->target, "vendor_boot") != 0)
            puts("OPTIONAL:vendor_boot");
        return;
    }

    print_rich(res);
}
