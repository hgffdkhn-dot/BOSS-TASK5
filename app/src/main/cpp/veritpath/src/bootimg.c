/* veritpath - Android boot image analyzer and payload injector.
 *
 * _GNU_SOURCE is defined here rather than on the command line so the sources
 * compile identically under -std=c11, -std=gnu11 and any cross toolchain:
 * glibc otherwise hides PATH_MAX, strtok_r, symlink, readlink and lstat.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

/* Android boot image headers v0..v4 and vendor_boot v3..v4. */
#include "vp.h"

#include <stdlib.h>

#define BOOT_MAGIC "ANDROID!"
#define VENDOR_MAGIC "VNDRBOOT"
#define DTB_MAGIC_BE 0xd00dfeedu

static uint32_t rd32(const uint8_t *p, size_t off)
{
    return (uint32_t)(p[off] | (p[off + 1] << 8) | (p[off + 2] << 16) |
                      ((uint32_t)p[off + 3] << 24));
}

static uint64_t rd64(const uint8_t *p, size_t off)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | p[off + i];
    return v;
}

static void wr32(uint8_t *p, size_t off, uint32_t v)
{
    p[off] = (uint8_t)(v & 0xff);
    p[off + 1] = (uint8_t)((v >> 8) & 0xff);
    p[off + 2] = (uint8_t)((v >> 16) & 0xff);
    p[off + 3] = (uint8_t)((v >> 24) & 0xff);
}

static void wr64(uint8_t *p, size_t off, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[off + i] = (uint8_t)((v >> (8 * i)) & 0xff);
}

static size_t boot_header_size(uint32_t hv)
{
    switch (hv) {
    case 0: return 1632;
    case 1: return 1648;
    case 2: return 1660;
    case 3: return 1580;
    default: return 1584;
    }
}

static size_t vendor_header_size(uint32_t hv)
{
    return hv >= 4 ? 2128 : 2112;
}

void boot_img_init(boot_img_t *img)
{
    memset(img, 0, sizeof(*img));
    buf_init(&img->kernel);
    buf_init(&img->ramdisk);
    buf_init(&img->second);
    buf_init(&img->dtb);
    buf_init(&img->recovery_dtbo);
    buf_init(&img->boot_signature);
    buf_init(&img->bootconfig);
    buf_init(&img->raw_header);
    buf_init(&img->unwrapped);
    img->page_size = 4096;
}

void boot_img_free(boot_img_t *img)
{
    buf_free(&img->kernel);
    buf_free(&img->ramdisk);
    buf_free(&img->second);
    buf_free(&img->dtb);
    buf_free(&img->recovery_dtbo);
    buf_free(&img->boot_signature);
    buf_free(&img->bootconfig);
    buf_free(&img->raw_header);
    buf_free(&img->unwrapped);
    for (size_t i = 0; i < img->n_frags; i++)
        free(img->frags[i].board_id);
    free(img->frags);
    free(img->chunk_fmts);
    memset(img, 0, sizeof(*img));
}

#define MAX_MAGIC_SEARCH (1u << 20)

/* Wrappers that are definitely not raw boot images.  Checked before scanning
 * for the magic: a sparse image or payload.bin carries boot data (and thus the
 * magic) somewhere inside, but parsing it as a boot image yields nonsense. */
static const char *identify_container(const uint8_t *d, size_t len)
{
    static const uint8_t sparse[4] = {0x3a, 0xff, 0x26, 0xed};
    static const uint8_t dtb[4] = {0xd0, 0x0d, 0xfe, 0xed};
    if (len >= 4 && memcmp(d, sparse, 4) == 0)
        return "sparse";
    if (len >= 4 && memcmp(d, "CrAU", 4) == 0)
        return "payload";
    if (len >= 4 && memcmp(d, "PK\x03\x04", 4) == 0)
        return "zip";
    if (len >= 4 && memcmp(d, "AVB0", 4) == 0)
        return "avb";
    if (len >= 4 && memcmp(d, "\x7f" "ELF", 4) == 0)
        return "elf";
    if (len >= 4 && memcmp(d, dtb, 4) == 0)
        return "dtb";
    return NULL;
}

static void report_container(const char *kind, size_t len, const char *path)
{
    const char *name = path ? path : "boot.img";
    vp_err("%s: not an Android boot image", name);
    fprintf(stderr, "  size : %zu bytes\n\n", len);
    if (strcmp(kind, "sparse") == 0) {
        fprintf(stderr, "  this is an Android *sparse* image - unpack it first:\n");
        fprintf(stderr, "      simg2img %s %s.raw\n\n", name, name);
    } else if (strcmp(kind, "payload") == 0) {
        fprintf(stderr, "  this is a ChromeOS/A/B payload.bin - extract boot first:\n");
        fprintf(stderr, "      payload-dumper-go payload.bin --part boot\n");
    } else if (strcmp(kind, "zip") == 0) {
        fprintf(stderr, "  this is a ZIP archive - extract boot.img first:\n");
        fprintf(stderr, "      unzip %s\n\n", name);
    } else if (strcmp(kind, "avb") == 0) {
        fprintf(stderr, "  this is an AVB structure, not a boot image\n");
        fprintf(stderr, "      dump the boot/init_boot partition instead of vbmeta\n");
    } else if (strcmp(kind, "elf") == 0) {
        fprintf(stderr, "  this is an ELF file (raw kernel / vmlinux), not a boot image\n");
        fprintf(stderr, "      supply the boot.img that wraps it\n");
    } else {
        fprintf(stderr, "  this is a bare device tree blob (dtb), not a boot image\n");
        fprintf(stderr, "      supply boot.img / init_boot.img instead\n");
    }
}

/* How plausible is an Android header at `off`?  A large dump can contain the
 * magic inside unrelated data, so score every candidate and take the best. */
static int plausible_page(uint32_t page)
{
    if (page == 0 || page == 2048 || page == 4096 || page == 8192 ||
        page == 16384 || page == 32768 || page == 65536 || page == 131072)
        return 1;
    return (page & (page - 1)) == 0 && page >= 2048 && page <= 131072;
}

static int header_score(const uint8_t *d, size_t len, size_t off)
{
    if (off + 64 > len)
        return -1;
    if (memcmp(d + off, VENDOR_MAGIC, 8) == 0) {
        uint32_t hv = rd32(d, off + 8);
        uint32_t page = rd32(d, off + 12);
        int score = 90;
        if (hv == 3 || hv == 4)
            score += 40;
        if (plausible_page(page ? page : 4096))
            score += 20;
        return score;
    }
    if (memcmp(d + off, BOOT_MAGIC, 8) != 0)
        return -1;
    uint32_t hv24 = rd32(d, off + 24);
    uint32_t hs20 = rd32(d, off + 20);
    uint32_t hv40 = rd32(d, off + 40);
    uint32_t page = rd32(d, off + 36);
    int score = 0;
    if (hv24 == 3 || hv24 == 4 || hv24 == 5 || hv24 == 6) {
        score += 80;
        if (hs20 == 0 || (hs20 >= 1500 && hs20 <= 4096))
            score += 20;
    }
    if (hv40 <= 2) {
        score += 60;
        if (plausible_page(page))
            score += 25;
    }
    if (score == 0)
        return 0;
    uint32_t ksize = rd32(d, off + 8);
    uint32_t rsize = rd32(d, off + 12);
    uint32_t total = ksize + rsize;
    if (!(hv24 == 3 || hv24 == 4 || hv24 == 5 || hv24 == 6))
        total += rd32(d, off + 24);
    if ((size_t)total <= len - off)
        score += 15;
    else if ((size_t)total > len)
        score -= 40;
    return score;
}

static int find_magic(const uint8_t *d, size_t len, size_t *off)
{
    /* 1 = ANDROID!, 2 = VNDRBOOT, 0 = not found */
    if (len >= 8) {
        if (memcmp(d, VENDOR_MAGIC, 8) == 0) {
            *off = 0;
            return 2;
        }
        if (memcmp(d, BOOT_MAGIC, 8) == 0) {
            *off = 0;
            return 1;
        }
    }
    size_t limit = len < MAX_MAGIC_SEARCH ? len : MAX_MAGIC_SEARCH;
    int best_score = 1; /* require a positive score */
    int which = 0;
    size_t best_off = 0;
    static const struct {
        const char *magic;
        int id;
    } cand[] = {{BOOT_MAGIC, 1}, {VENDOR_MAGIC, 2}};
    for (int c = 0; c < 2; c++) {
        for (size_t i = 0; i + 8 <= limit; i++) {
            if (memcmp(d + i, cand[c].magic, 8) != 0)
                continue;
            int score = header_score(d, len, i);
            if (score > best_score) {
                best_score = score;
                best_off = i;
                which = cand[c].id;
            }
        }
    }
    if (which)
        *off = best_off;
    return which;
}

static void print_hex_head(const uint8_t *d, size_t len)
{
    size_t n = len < 16 ? len : 16;
    char hex[16 * 3 + 1];
    char txt[17];
    size_t p = 0;
    for (size_t i = 0; i < n; i++) {
        p += (size_t)snprintf(hex + p, sizeof(hex) - p, "%02x ", d[i]);
        txt[i] = (d[i] >= 32 && d[i] < 127) ? (char)d[i] : '.';
    }
    txt[n] = 0;
    fprintf(stderr, "  head          : %s |%s|\n", hex, txt);
}

static void report_no_magic(const uint8_t *d, size_t len, const char *path)
{
    vp_err("%s: not an Android boot image (no ANDROID!/VNDRBOOT magic)",
           path ? path : "<memory>");
    fprintf(stderr, "  size          : %zu bytes\n", len);
    print_hex_head(d, len);
    static const uint8_t sparse[4] = {0x3a, 0xff, 0x26, 0xed};
    if (len >= 4 && memcmp(d, sparse, 4) == 0) {
        fprintf(stderr, "  this is an Android *sparse* image - convert it first:\n");
        fprintf(stderr, "      simg2img %s %s.raw\n", path ? path : "boot.img",
                path ? path : "boot.img");
    } else if (len == 0) {
        fprintf(stderr, "  the file is empty - the dd / download probably failed\n");
    } else if (len >= 4 && memcmp(d, "CrAU", 4) == 0) {
        fprintf(stderr, "  this is a ChromeOS/A/B payload.bin - extract boot first:\n");
        fprintf(stderr, "      payload-dumper-go payload.bin --part boot\n");
    } else if (len >= 2 && d[0] == 'P' && d[1] == 'K') {
        fprintf(stderr, "  this is a ZIP archive - extract boot.img first\n");
    } else {
        fprintf(stderr, "  common causes:\n");
        fprintf(stderr, "    * wrong partition dumped (check /dev/block/by-name/)\n");
        fprintf(stderr, "    * the image is compressed - gunzip / xz -d it first\n");
        fprintf(stderr, "    * a partial or truncated dump\n");
    }
}

static void report_bad_version(const uint8_t *d, size_t len, const char *path)
{
    vp_err("%s: cannot determine boot image header version", path ? path : "<memory>");
    fprintf(stderr, "  size          : %zu bytes\n", len);
    fprintf(stderr, "  hdr_size  @20 : %u\n", rd32(d, 20));
    fprintf(stderr, "  hdr_ver   @24 : %u\n", rd32(d, 24));
    fprintf(stderr, "  page_size @36 : %u\n", rd32(d, 36));
    fprintf(stderr, "  hdr_ver   @40 : %u\n", rd32(d, 40));
    print_hex_head(d, len);
    fprintf(stderr, "  expected: v0-v2 -> version 0/1/2 at @40, sane page size at @36\n");
    fprintf(stderr, "            v3/v4 -> version 3/4 at @24, header size at @20\n");
}

/* Recover the header version from the layout itself: try every version and
 * keep the one whose ramdisk window actually holds a cpio archive (or a
 * compressed one).  A big GKI 1.0 boot.img must never be rejected just
 * because its header fields are unusual. */
static int ramdisk_window(const uint8_t *d, size_t len, int hv,
                          size_t *off_out, uint32_t *size_out)
{
    uint32_t ksize = rd32(d, 8);
    if (hv <= 2) {
        uint32_t page = rd32(d, 36);
        if (!plausible_page(page))
            page = 2048;
        if (!page)
            page = 2048;
        size_t hs = boot_header_size((uint32_t)hv);
        if (hv >= 1 && len >= 1648) {
            uint32_t declared = rd32(d, 1644);
            if (declared == 1632 || declared == 1648 || declared == 1660)
                hs = declared;
        }
        size_t span = hs;
        if (span < page)
            span = page;
        span = round_up_sz(span, page);
        *off_out = round_up_sz(span + ksize, page);
        *size_out = rd32(d, 16);
    } else {
        uint32_t declared = rd32(d, 20);
        size_t hs = boot_header_size((uint32_t)hv);
        if (declared >= 1500 && declared <= 4096)
            hs = declared;
        size_t span = round_up_sz(hs, 4096);
        *off_out = round_up_sz(span + ksize, 4096);
        *size_out = rd32(d, 12);
    }
    return *size_out != 0 && *off_out + (size_t)*size_out <= len;
}

static int looks_like_ramdisk(const uint8_t *d, size_t len)
{
    if (!d || !len)
        return 0;
    if (len >= 6 && (memcmp(d, "070701", 6) == 0 || memcmp(d, "070702", 6) == 0))
        return 1;
    return comp_detect(d, len) != FMT_RAW;
}

static int infer_header_version(const uint8_t *d, size_t len, const char **why)
{
    static const int order[] = {0, 1, 2, 4, 3};
    for (int i = 0; i < 5; i++) {
        size_t off;
        uint32_t size;
        if (!ramdisk_window(d, len, order[i], &off, &size))
            continue;
        if (looks_like_ramdisk(d + off, size)) {
            *why = "ramdisk payload decodes for this layout";
            return order[i];
        }
    }
    for (int i = 0; i < 5; i++) {
        size_t off;
        uint32_t size;
        if (ramdisk_window(d, len, order[i], &off, &size)) {
            *why = "kernel+ramdisk sizes fit this layout";
            return order[i];
        }
    }
    *why = NULL;
    return -1;
}

int vp_forced_header_version = -1;

static uint32_t detect_header_version(const uint8_t *d, size_t len, const char *path);

uint32_t vp_detect_header_version(const uint8_t *d, size_t len, const char *path)
{
    return detect_header_version(d, len, path);
}

static uint32_t detect_header_version(const uint8_t *d, size_t len, const char *path)
{
    if (vp_forced_header_version >= 0)
        return (uint32_t)vp_forced_header_version;
    if (len < 64) {
        report_bad_version(d, len, path);
        return 0xFFFFFFFFu;
    }
    if (memcmp(d, VENDOR_MAGIC, 8) == 0) {
        uint32_t hv = rd32(d, 8);
        return (hv == 3 || hv == 4) ? hv : 3;
    }
    uint32_t hv24 = rd32(d, 24);
    uint32_t hv40 = rd32(d, 40);

    /* Unambiguous markers first.  Offset 24 of a legacy header is second_size
     * and a 3..6 byte second stage cannot exist, so this cannot collide; and
     * offset 40 of a v3/v4 header sits inside the cmdline, which is exactly
     * why the v3/v4 test has to run before the legacy one. */
    if (hv24 == 3 || hv24 == 4 || hv24 == 5 || hv24 == 6)
        return hv24;
    if (hv40 <= 2)
        return hv40;

    /* Both markers are garbage: recover the version from the layout instead of
     * refusing an image that is perfectly fine. */
    const char *why = NULL;
    int guess = infer_header_version(d, len, &why);
    if (guess >= 0) {
        vp_warn("%s: header fields unrecognised, recovered v%d from the layout (%s)",
                path ? path : "image", guess, why ? why : "?");
        return (uint32_t)guess;
    }
    report_bad_version(d, len, path);
    fprintf(stderr, "  force a version instead:  veritpath analyze --boot %s "
                    "--header-version 3\n", path ? path : "boot.img");
    return 0xFFFFFFFFu;
}

static char *copy_cstr(const uint8_t *src, size_t max)
{
    size_t n = 0;
    while (n < max && src[n])
        n++;
    char *out = xmalloc(n + 1);
    memcpy(out, src, n);
    out[n] = 0;
    return out;
}

static void store_cstr(const uint8_t *src, size_t max, char *dst, size_t dstsz)
{
    size_t n = 0;
    while (n < max && src[n])
        n++;
    if (n >= dstsz)
        n = dstsz - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static int parse_legacy(const uint8_t *d, size_t len, boot_img_t *img)
{
    uint32_t ksize = rd32(d, 8);
    uint32_t rsize = rd32(d, 16);
    uint32_t ssize = rd32(d, 24);
    img->page_size = rd32(d, 36);
    if (!img->page_size)
        img->page_size = 2048;
    img->os_version = rd32(d, 44);
    store_cstr(d + 48, 16, img->name, sizeof(img->name));
    store_cstr(d + 64, 512, img->cmdline_main, sizeof(img->cmdline_main));
    store_cstr(d + 608, 1024, img->cmdline_extra, sizeof(img->cmdline_extra));
    size_t hsize = boot_header_size(img->header_version);
    if (img->header_version >= 1) {
        uint32_t declared = rd32(d, 1644);
        if (declared == 1632 || declared == 1648 || declared == 1660)
            hsize = declared;
    }
    img->header_span = img->page_size > round_up_sz(hsize, img->page_size)
                           ? img->page_size
                           : round_up_sz(hsize, img->page_size);
    buf_append(&img->raw_header, d, img->header_span > len ? len : img->header_span);

    size_t page = img->page_size;
    size_t off = img->header_span;
    if (off + ksize <= len)
        buf_append(&img->kernel, d + off, ksize);
    off = round_up_sz(off + ksize, page);
    if (off + rsize <= len)
        buf_append(&img->ramdisk, d + off, rsize);
    off = round_up_sz(off + rsize, page);
    if (ssize && off + ssize <= len)
        buf_append(&img->second, d + off, ssize);
    off = round_up_sz(off + ssize, page);
    if (img->header_version >= 1) {
        uint32_t rdsize = rd32(d, 1632);
        uint64_t rdoff = rd64(d, 1636);
        if (!rdoff)
            rdoff = off;
        if (rdsize && rdoff + rdsize <= len)
            buf_append(&img->recovery_dtbo, d + rdoff, rdsize);
        off = round_up_sz(rdoff + rdsize, page);
    }
    if (img->header_version >= 2) {
        uint32_t dtbsize = rd32(d, 1648);
        if (dtbsize && off + dtbsize <= len)
            buf_append(&img->dtb, d + off, dtbsize);
    }
    return 0;
}

static int parse_v3(const uint8_t *d, size_t len, boot_img_t *img)
{
    uint32_t ksize = rd32(d, 8);
    uint32_t rsize = rd32(d, 12);
    img->os_version = rd32(d, 16);
    uint32_t declared = rd32(d, 20);
    uint32_t on_disk = rd32(d, 24);
    if (on_disk == 3 || on_disk == 4 || on_disk == 5 || on_disk == 6)
        img->header_version = on_disk;
    size_t hsize = boot_header_size(img->header_version);
    /* vendor tools write page-padded / zeroed / garbage header sizes */
    if (declared >= 1500 && declared <= 4096)
        hsize = declared;
    store_cstr(d + 28, 1536, img->cmdline, sizeof(img->cmdline));
    img->page_size = 4096;
    img->header_span = round_up_sz(hsize, img->page_size);
    if (img->header_span > len)
        img->header_span = len;
    buf_append(&img->raw_header, d, img->header_span);

    size_t off = img->header_span;
    if (off + ksize <= len)
        buf_append(&img->kernel, d + off, ksize);
    off = round_up_sz(off + ksize, img->page_size);
    if (off + rsize <= len)
        buf_append(&img->ramdisk, d + off, rsize);
    off = round_up_sz(off + rsize, img->page_size);
    if (img->header_version >= 4) {
        uint32_t sigsize = rd32(d, 1564);
        if (sigsize && off + sigsize <= len)
            buf_append(&img->boot_signature, d + off, sigsize);
    }
    /* v3/v4 keep the dtb appended to the kernel image */
    for (size_t i = 0; i + 4 <= img->kernel.len; i += 4) {
        if (rd32(img->kernel.data, i) == DTB_MAGIC_BE) {
            buf_append(&img->dtb, img->kernel.data + i, img->kernel.len - i);
            break;
        }
    }
    return 0;
}

static int parse_vendor(const uint8_t *d, size_t len, boot_img_t *img)
{
    img->is_vendor = 1;
    img->header_version = rd32(d, 8);
    img->page_size = rd32(d, 12);
    if (!img->page_size)
        img->page_size = 4096;
    uint32_t rsize = rd32(d, 24);
    store_cstr(d + 28, 2048, img->cmdline, sizeof(img->cmdline));
    store_cstr(d + 2080, 16, img->name, sizeof(img->name));
    uint32_t dtbsize = rd32(d, 2096);
    size_t hsize = vendor_header_size(img->header_version);
    img->header_span = round_up_sz(hsize, img->page_size);
    if (img->header_span > len)
        img->header_span = len;
    buf_append(&img->raw_header, d, img->header_span);

    size_t page = img->page_size;
    size_t off = img->header_span;
    if (off + rsize <= len)
        buf_append(&img->ramdisk, d + off, rsize);
    off = round_up_sz(off + rsize, page);
    if (dtbsize && off + dtbsize <= len)
        buf_append(&img->dtb, d + off, dtbsize);
    off = round_up_sz(off + dtbsize, page);
    if (img->header_version >= 4) {
        uint32_t tsize = rd32(d, 2108);
        uint32_t tnum = rd32(d, 2112);
        uint32_t esize = rd32(d, 2116);
        uint32_t bcsize = rd32(d, 2120);
        if (!esize)
            esize = 108;
        img->frag_entry_size = esize;
        if (tsize && tnum && off + tsize <= len && esize >= 44) {
            img->frags = xmalloc(sizeof(vendor_fragment_t) * tnum);
            for (uint32_t i = 0; i < tnum; i++) {
                size_t base = off + (size_t)i * esize;
                if (base + esize > len)
                    break;
                vendor_fragment_t f;
                memset(&f, 0, sizeof(f));
                f.size = rd32(d, base);
                f.offset = rd32(d, base + 4);
                f.type = rd32(d, base + 8);
                char *nm = copy_cstr(d + base + 12, 32);
                snprintf(f.name, sizeof(f.name), "%s", nm);
                free(nm);
                f.board_id_len = esize - 44;
                f.board_id = xmalloc(f.board_id_len ? f.board_id_len : 1);
                memcpy(f.board_id, d + base + 44, f.board_id_len);
                img->frags[img->n_frags++] = f;
            }
        }
        off = round_up_sz(off + tsize, page);
        if (bcsize && off + bcsize <= len)
            buf_append(&img->bootconfig, d + off, bcsize);
    }
    return 0;
}

int boot_img_parse(const uint8_t *data, size_t len, const char *role,
                   const char *path, boot_img_t *img)
{
    boot_img_init(img);
    if (role)
        snprintf(img->role, sizeof(img->role), "%s", role);
    img->path = path;

    size_t off = 0;
    const char *container = identify_container(data, len);
    if (container) {
        report_container(container, len, img->path);
        return -1;
    }
    int magic = find_magic(data, len, &off);
    if (magic == 0) {
        /* maybe it is a compressed boot.img (some ROM zips ship boot.img.gz) */
        buf_t raw;
        buf_init(&raw);
        if (comp_decompress(data, len, &raw) == 0 && raw.len >= 64) {
            size_t off2 = 0;
            int magic2 = find_magic(raw.data, raw.len, &off2);
            if (magic2) {
                img->unwrapped = raw;
                img->header_offset = off2;
                data = raw.data + off2;
                len = raw.len - off2;
                magic = magic2;
            } else {
                buf_free(&raw);
            }
        } else {
            buf_free(&raw);
        }
    }
    if (magic == 0) {
        report_no_magic(data, len, img->path);
        return -1;
    }
    if (off) {
        img->header_offset = off;
        vp_warn("%s: ANDROID! header found at offset %zu - skipping a prefix",
                img->path ? img->path : "<memory>", off);
        data += off;
        len -= off;
    }

    if (magic == 2)
        return parse_vendor(data, len, img);
    img->header_version = detect_header_version(data, len, img->path);
    if (img->header_version == 0xFFFFFFFFu)
        return -1;
    if (img->header_version <= 2)
        return parse_legacy(data, len, img);
    return parse_v3(data, len, img);
}

const char *boot_img_cmdline(boot_img_t *img)
{
    if (img->is_vendor || img->header_version >= 3)
        return img->cmdline;
    static char joined[1600];
    snprintf(joined, sizeof(joined), "%s %s", img->cmdline_main, img->cmdline_extra);
    return joined;
}

static int cmdline_room_left(boot_img_t *img, size_t *cap)
{
    if (img->is_vendor) {
        *cap = 2047;
        return 0;
    }
    if (img->header_version >= 3) {
        *cap = 1535;
        return 0;
    }
    *cap = 511; /* main cmdline first; caller falls back to extra */
    return 0;
}

int boot_img_append_cmdline(boot_img_t *img, const char *extra)
{
    if (!extra || !*extra)
        return 0;
    const char *cur = boot_img_cmdline(img);
    /* avoid duplicates */
    char tmp[1600];
    snprintf(tmp, sizeof(tmp), "%s", extra);
    char *save = NULL;
    int changed = 0;
    for (char *tok = strtok_r(tmp, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
        if (strstr(cur, tok))
            continue;
        if (img->is_vendor || img->header_version >= 3) {
            size_t cap = 0;
            cmdline_room_left(img, &cap);
            size_t used = strlen(img->cmdline);
            if (used + strlen(tok) + 2 > cap)
                return -1;
            if (used)
                strncat(img->cmdline, " ", sizeof(img->cmdline) - used - 1);
            strncat(img->cmdline, tok, sizeof(img->cmdline) - strlen(img->cmdline) - 1);
        } else {
            size_t used = strlen(img->cmdline_main);
            if (used + strlen(tok) + 2 <= 512) {
                if (used)
                    strncat(img->cmdline_main, " ",
                            sizeof(img->cmdline_main) - used - 1);
                strncat(img->cmdline_main, tok,
                        sizeof(img->cmdline_main) - strlen(img->cmdline_main) - 1);
            } else {
                used = strlen(img->cmdline_extra);
                if (used + strlen(tok) + 2 > 1024)
                    return -1;
                if (used)
                    strncat(img->cmdline_extra, " ",
                            sizeof(img->cmdline_extra) - used - 1);
                strncat(img->cmdline_extra, tok,
                        sizeof(img->cmdline_extra) - strlen(img->cmdline_extra) - 1);
            }
        }
        changed = 1;
    }
    return changed ? 0 : 0;
}

int boot_img_has_ramdisk(boot_img_t *img)
{
    return img->ramdisk.len > 0;
}

buf_t *boot_img_dtb(boot_img_t *img)
{
    return img->dtb.len ? &img->dtb : NULL;
}

int boot_img_ramdisk_archive(boot_img_t *img, cpio_archive_t *a)
{
    if (!img->ramdisk.len)
        return -1;
    comp_chunks_t chunks;
    if (comp_split(img->ramdisk.data, img->ramdisk.len, &chunks) != 0)
        return -1;
    buf_t raw;
    buf_init(&raw);
    /* the archive may be read more than once (detection, then injection) */
    free(img->chunk_fmts);
    img->chunk_fmts = xmalloc(sizeof(comp_fmt_t) * chunks.n);
    img->n_chunks = chunks.n;
    for (size_t i = 0; i < chunks.n; i++) {
        img->chunk_fmts[i] = chunks.items[i].fmt;
        buf_append(&raw, chunks.items[i].data.data, chunks.items[i].data.len);
    }
    int rc = cpio_parse(raw.data, raw.len, a);
    buf_free(&raw);
    comp_chunks_free(&chunks);
    return rc;
}

int boot_img_set_ramdisk(boot_img_t *img, cpio_archive_t *a, int force)
{
    buf_t *segs = xmalloc(sizeof(buf_t) * (a->n ? a->n : 1));
    for (size_t i = 0; i < a->n; i++) {
        buf_init(&segs[i]);
        cpio_serialize_seg(&a->segs[i], &segs[i]);
    }
    buf_t out;
    buf_init(&out);
    uint32_t *csizes = xmalloc(sizeof(uint32_t) * (a->n ? a->n : 1));
    memset(csizes, 0, sizeof(uint32_t) * (a->n ? a->n : 1));

    if (a->n > 1 && (force >= 0 || img->n_chunks == a->n)) {
        /* every segment keeps its own compression (vendor_boot fragments) */
        for (size_t i = 0; i < a->n; i++) {
            buf_t c;
            buf_init(&c);
            comp_fmt_t fmt = force >= 0 ? (comp_fmt_t)force : img->chunk_fmts[i];
            if (fmt == FMT_RAW)
                fmt = FMT_GZIP;
            comp_compress(segs[i].data, segs[i].len, fmt, &c);
            buf_append(&out, c.data, c.len);
            csizes[i] = (uint32_t)c.len;
            buf_free(&c);
        }
    } else {
        buf_t joined;
        buf_init(&joined);
        for (size_t i = 0; i < a->n; i++)
            buf_append(&joined, segs[i].data, segs[i].len);
        comp_fmt_t fmt =
            force >= 0 ? (comp_fmt_t)force
                      : (img->n_chunks ? img->chunk_fmts[0] : FMT_GZIP);
        if (fmt == FMT_RAW)
            fmt = FMT_GZIP;
        comp_compress(joined.data, joined.len, fmt, &out);
        buf_free(&joined);
        if (a->n)
            csizes[0] = (uint32_t)out.len;
    }
    for (size_t i = 0; i < a->n; i++)
        buf_free(&segs[i]);
    free(segs);

    /* keep the vendor ramdisk fragment table in sync with the new sizes */
    if (img->is_vendor && img->n_frags == a->n && a->n > 0) {
        uint32_t delta = 0;
        for (size_t i = 0; i < img->n_frags; i++) {
            uint32_t old = img->frags[i].size;
            img->frags[i].offset += delta;
            img->frags[i].size = csizes[i];
            delta += csizes[i] - old;
        }
        if (a->n == 1) {
            /* one fragment: the single blob covers everything */
            img->frags[0].size = (uint32_t)out.len;
        }
    }
    free(csizes);

    buf_reset(&img->ramdisk);
    buf_append(&img->ramdisk, out.data, out.len);
    buf_free(&out);
    return 0;
}

int boot_img_pack(boot_img_t *img, buf_t *out)
{
    size_t page = img->is_vendor ? img->page_size : (img->header_version <= 2 ? img->page_size : 4096);
    buf_t header;
    buf_init(&header);
    if (img->raw_header.len) {
        buf_append(&header, img->raw_header.data, img->raw_header.len);
        size_t need = img->is_vendor ? vendor_header_size(img->header_version)
                                     : boot_header_size(img->header_version);
        if (header.len < need)
            buf_append_pad(&header, need - header.len, 0);
    } else {
        size_t hsize = img->is_vendor ? vendor_header_size(img->header_version)
                                      : boot_header_size(img->header_version);
        buf_append_pad(&header, hsize, 0);
        buf_append(&header, img->is_vendor ? VENDOR_MAGIC : BOOT_MAGIC, 8);
        if (img->is_vendor) {
            wr32(header.data, 8, img->header_version);
            wr32(header.data, 12, img->page_size);
        } else {
            wr32(header.data, 8, (uint32_t)img->kernel.len);
            if (img->header_version <= 2) {
                wr32(header.data, 36, img->page_size);
                wr32(header.data, 40, img->header_version);
                wr32(header.data, 44, img->os_version);
                if (img->header_version >= 1)
                    wr32(header.data, 1644, (uint32_t)hsize);
            } else {
                wr32(header.data, 20, (uint32_t)hsize);
                wr32(header.data, 24, img->header_version);
            }
        }
    }

    if (img->is_vendor) {
        wr32(header.data, 24, (uint32_t)img->ramdisk.len);
        wr32(header.data, 2096, (uint32_t)img->dtb.len);
        char cl[2048];
        memset(cl, 0, sizeof(cl));
        /* bound by the *source*, not the destination: cmdline is 1600 bytes,
         * so asking strnlen for 2047 would read past it */
        size_t clen = strnlen(img->cmdline, sizeof(img->cmdline));
        if (clen > sizeof(cl) - 1)
            clen = sizeof(cl) - 1;
        memcpy(cl, img->cmdline, clen);
        memcpy(header.data + 28, cl, 2048);
        if (img->header_version >= 4) {
            size_t tsize = img->n_frags * img->frag_entry_size;
            wr32(header.data, 2108, (uint32_t)tsize);
            wr32(header.data, 2112, (uint32_t)img->n_frags);
            wr32(header.data, 2116, img->frag_entry_size);
            wr32(header.data, 2120, (uint32_t)img->bootconfig.len);
        }
        buf_append(out, header.data, header.len);
        buf_append_pad(out, img->header_span ? img->header_span : page, 0);
        buf_append(out, img->ramdisk.data, img->ramdisk.len);
        buf_append_pad(out, page, 0);
        buf_append(out, img->dtb.data, img->dtb.len);
        buf_append_pad(out, page, 0);
        if (img->header_version >= 4) {
            for (size_t i = 0; i < img->n_frags; i++) {
                uint8_t *e = xmalloc(img->frag_entry_size);
                memset(e, 0, img->frag_entry_size);
                wr32(e, 0, img->frags[i].size);
                wr32(e, 4, img->frags[i].offset);
                wr32(e, 8, img->frags[i].type);
                memcpy(e + 12, img->frags[i].name, strlen(img->frags[i].name));
                if (img->frags[i].board_id && img->frag_entry_size > 44)
                    memcpy(e + 44, img->frags[i].board_id,
                           img->frag_entry_size - 44);
                buf_append(out, e, img->frag_entry_size);
                free(e);
            }
            buf_append_pad(out, page, 0);
            buf_append(out, img->bootconfig.data, img->bootconfig.len);
            buf_append_pad(out, page, 0);
        }
        buf_free(&header);
        return 0;
    }

    if (img->header_version <= 2) {
        wr32(header.data, 8, (uint32_t)img->kernel.len);
        wr32(header.data, 16, (uint32_t)img->ramdisk.len);
        wr32(header.data, 24, (uint32_t)img->second.len);
        char cl[512];
        memset(cl, 0, sizeof(cl));
        memcpy(cl, img->cmdline_main, strnlen(img->cmdline_main, sizeof(cl) - 1));
        memcpy(header.data + 64, cl, 512);
        char ex[1024];
        memset(ex, 0, sizeof(ex));
        memcpy(ex, img->cmdline_extra, strnlen(img->cmdline_extra, sizeof(ex) - 1));
        memcpy(header.data + 608, ex, 1024);
        if (img->header_version >= 2)
            wr32(header.data, 1648, (uint32_t)img->dtb.len);
        if (img->header_version >= 1) {
            size_t rdoff = img->header_span;
            rdoff = round_up_sz(rdoff + img->kernel.len, page);
            rdoff = round_up_sz(rdoff + img->ramdisk.len, page);
            rdoff = round_up_sz(rdoff + img->second.len, page);
            wr32(header.data, 1632, (uint32_t)img->recovery_dtbo.len);
            wr64(header.data, 1636, rdoff);
        }
    } else {
        wr32(header.data, 8, (uint32_t)img->kernel.len);
        wr32(header.data, 12, (uint32_t)img->ramdisk.len);
        wr32(header.data, 16, img->os_version);
        char cl[1536];
        memset(cl, 0, sizeof(cl));
        memcpy(cl, img->cmdline, strnlen(img->cmdline, sizeof(cl) - 1));
        memcpy(header.data + 28, cl, 1536);
        if (img->header_version >= 4)
            wr32(header.data, 1564, (uint32_t)img->boot_signature.len);
    }

    buf_append(out, header.data, header.len);
    buf_append_pad(out, img->header_span ? img->header_span : page, 0);
    buf_append(out, img->kernel.data, img->kernel.len);
    buf_append_pad(out, page, 0);
    buf_append(out, img->ramdisk.data, img->ramdisk.len);
    buf_append_pad(out, page, 0);
    if (img->header_version <= 2) {
        buf_append(out, img->second.data, img->second.len);
        buf_append_pad(out, page, 0);
        if (img->header_version >= 1) {
            buf_append(out, img->recovery_dtbo.data, img->recovery_dtbo.len);
            buf_append_pad(out, page, 0);
        }
        if (img->header_version >= 2) {
            buf_append(out, img->dtb.data, img->dtb.len);
            buf_append_pad(out, page, 0);
        }
    } else if (img->header_version >= 4) {
        buf_append(out, img->boot_signature.data, img->boot_signature.len);
        buf_append_pad(out, page, 0);
    }
    buf_free(&header);
    return 0;
}
