/* veritpath - Android boot image analyzer and payload injector.
 *
 * _GNU_SOURCE is defined here rather than on the command line so the sources
 * compile identically under -std=c11, -std=gnu11 and any cross toolchain:
 * glibc otherwise hides PATH_MAX, strtok_r, symlink, readlink and lstat.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

/* Counter-measure plan printing. */
#include "compat.h"
#include "vp.h"

#include <stdlib.h>

void options_init(options_t *o)
{
    memset(o, 0, sizeof(*o));
    o->segment = -1;
    o->ramdisk_format = (comp_fmt_t)-1;
}

static const char *layout_advice(const char *layout)
{
    if (strcmp(layout, "init_boot") == 0)
        return "Android 13+ GKI: the generic ramdisk moved to init_boot.img while "
               "boot.img only carries the kernel. Patch init_boot.img.";
    if (strcmp(layout, "vendor_boot") == 0)
        return "GKI 1.0 (Android 11/12): boot.img has no ramdisk, the generic "
               "ramdisk is the vendor_ramdisk inside vendor_boot.img. Patch it "
               "there; the fragment table is rebuilt automatically.";
    if (strcmp(layout, "boot") == 0)
        return "Ramdisk lives inside this image (Android <= 12 non-GKI). Patch it "
               "directly; kernel and DTB stay untouched.";
    return "No ramdisk located. Supply init_boot.img / vendor_boot.img (Android "
           "13+ GKI), or use --create-ramdisk to build one for a system-as-root "
           "device that boots without an initramfs.";
}

void strategy_print_plan(analysis_t *res, payload_t *p, options_t *o, int as_json)
{
    if (as_json) {
        printf("{\n");
        printf("  \"target\": \"%s\",\n", res->target);
        printf("  \"arch\": \"%s\",\n", res->arch);
        printf("  \"android_api\": %d,\n", res->android_api);
        printf("  \"system_as_root\": %s,\n", res->system_as_root ? "true" : "false");
        printf("  \"gki\": %s\n", res->gki ? "true" : "false");
        printf("}\n");
        return;
    }
    puts("veritpath injection plan");
    puts("============================================================");
    printf("arch            : %s\n", res->arch);
    printf("android         : %s (API %d)\n", res->android_version, res->android_api);
    printf("ramdisk layout  : %s\n", res->layout);
    printf("system-as-root  : %s\n", res->system_as_root ? "true" : "false");
    printf("GKI             : %s\n", res->gki ? "true" : "false");
    printf("target image    : %s\n", res->target[0] ? res->target : "(none)");
    if (res->needs_ramdisk && !o->create_ramdisk)
        printf("  - no ramdisk exists; --create-ramdisk is required for this "
               "image\n");
    if (res->needs_ramdisk && o->create_ramdisk)
        printf("  - create a ramdisk (directories, init.rc, file_contexts), then "
               "apply the payload\n");
    if (o->patch_vendor_boot)
        puts("extra targets   : vendor_boot");
    puts("");
    puts("counter-measures");
    puts("------------------------------------------------------------");
    printf("* %s\n", layout_advice(res->layout));

    int step = 1;
    if (res->n_segments > 1)
        printf("%2d. [ramdisk-segment] ramdisk has %d cpio segments -> inject into "
               "the main one\n",
               step++, res->n_segments);
    else
        printf("%2d. [ramdisk-segment] single cpio segment ramdisk\n", step++);

    if (o->ramdisk_format >= 0)
        printf("%2d. [ramdisk-compression] re-compress with %s (forced)\n", step++,
               comp_name((comp_fmt_t)o->ramdisk_format));
    else if (res->target_img && res->target_img->ramdisk.len)
        printf("%2d. [ramdisk-compression] keep the original %s compression\n",
               step++, comp_name(comp_detect(res->target_img->ramdisk.data,
                                             res->target_img->ramdisk.len)));

    if (p) {
        for (int i = 0; i < p->n_files; i++) {
            printf("%2d. [payload-file] %s -> %s (mode %04o)", step++, p->files[i].src,
                   p->files[i].dest, p->files[i].mode);
            if (p->files[i].backup_as[0])
                printf(", original kept as %s", p->files[i].backup_as);
            putchar('\n');
        }
        if (p->has_rc)
            printf("%2d. [init-rc] write %s and import it from %s\n", step++, p->rc.file,
                   p->rc.n_import ? p->rc.import_into[0] : "/init.rc");
        if (p->cmdline_append[0])
            printf("%2d. [cmdline] append: %s\n", step++, p->cmdline_append);
    }
    if (o->permissive)
        printf("%2d. [selinux] append androidboot.selinux=permissive to the kernel "
               "cmdline\n",
               step++);
    else
        printf("%2d. [selinux] leave SELinux enforcing; the payload must reuse an "
               "existing domain\n",
               step++);
    if (o->cmdline && o->cmdline[0])
        printf("%2d. [cmdline] append: %s\n", step++, o->cmdline);
    if (o->patch_vendor_boot)
        printf("%2d. [vendor-boot] also patch every vendor_ramdisk fragment "
               "(platform + recovery)\n",
               step++);

    puts("");
    puts("notes");
    puts("------------------------------------------------------------");
    if (res->system_as_root)
        puts("* system-as-root: ramdisk files vanish after init switches root to "
             "/system - have your rc copy them out on post-fs-data.");
    else
        puts("* legacy root layout: injected files stay visible in / after boot.");
    if (res->gki)
        puts("* GKI device: never touch the kernel - only the ramdisk is patched.");
    if (res->has_vendor_boot && !o->patch_vendor_boot && res->recovery_fragment)
        puts("* vendor_boot.img present: add --patch-vendor-boot if you need the "
             "payload inside recovery/fastbootd.");

    puts("");
    puts("warnings");
    puts("------------------------------------------------------------");
    puts("! the patched image is no longer signed by the OEM key: disable "
         "dm-verity or re-sign");
    if (res->already_patched && !o->force)
        puts("! image already contains a veritpath payload; pass --force to re-inject");

    puts("");
    puts("flash");
    puts("------------------------------------------------------------");
    const char *fn = strcmp(res->target, "vendor_boot") == 0 ? "vendor_boot" : "boot";
    if (strcmp(res->target, "init_boot") == 0)
        fn = "init_boot";
    if (res->slot[0] == 'a' || res->slot[0] == 'b')
        printf("  fastboot flash %s_%c %s.veritpath.img\n", fn, res->slot[0], fn);
    else
        printf("  fastboot flash %s %s.veritpath.img\n", fn, fn);
    puts("  fastboot --disable-verity --disable-verification flash vbmeta vbmeta.img");
    puts("  fastboot reboot");
}
