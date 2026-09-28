/* veritpath - Android boot image analyzer and payload injector
 *
 * Single public header shared by all translation units.
 */
#ifndef VP_H
#define VP_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VP_VERSION "0.2.0"

/* ---------------------------------------------------------------- logging */

extern int vp_verbose;
extern int vp_forced_header_version;
void vp_set_verbose(int on);
void vp_log(const char *fmt, ...);      /* "==> " */
void vp_info(const char *fmt, ...);     /* "  . " */
void vp_warn(const char *fmt, ...);     /* "  ! " */
void vp_dbg(const char *fmt, ...);
void vp_err(const char *fmt, ...);
void vp_report_missing(const char *role, const char *path);

/* ------------------------------------------------------------- utilities */

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} buf_t;

void buf_init(buf_t *b);
void buf_free(buf_t *b);
void buf_reset(buf_t *b);
int buf_reserve(buf_t *b, size_t extra);
int buf_append(buf_t *b, const void *data, size_t len);
int buf_append_str(buf_t *b, const char *s);
int buf_appendf(buf_t *b, const char *fmt, ...);
int buf_append_pad(buf_t *b, size_t align, uint8_t fill);

int read_file(const char *path, buf_t *out);
int write_file(const char *path, const void *data, size_t len);
int is_dir(const char *path);
int file_exists(const char *path);
int mkdir_p(const char *path);
size_t round_up_sz(size_t v, size_t align);
const char *human_size(size_t n);       /* static rotating buffer */
void *vp_memmem(const void *hay, size_t haylen, const void *needle, size_t needlelen);
char *path_join(const char *a, const char *b);
char *path_dirname(const char *path);
int dir_in_path(const char *dir, const char *path);

/* run the CLI without process exit: for embedders (JNI, tests, other tools) */
void vp_set_program_name(const char *name);
int vp_cli_run(int argc, char **argv);

/* capture what vp_cli_run prints, as a malloc'd string */
int vp_capture_start(void);
char *vp_capture_stop(void);
char *replace_suffix(const char *path, const char *suffix); /* stem + suffix */

/* ---------------------------------------------------------- compression */

typedef enum {
    FMT_RAW = 0,
    FMT_GZIP,
    FMT_XZ,
    FMT_LZMA,
    FMT_LZ4,
    FMT_LZ4_LEGACY,
    FMT_BZIP2,
    FMT_ZSTD
} comp_fmt_t;

comp_fmt_t comp_detect(const uint8_t *data, size_t len);
const char *comp_name(comp_fmt_t f);
comp_fmt_t comp_from_name(const char *name);
int comp_decompress(const uint8_t *in, size_t inlen, buf_t *out);
int comp_compress(const uint8_t *in, size_t inlen, comp_fmt_t fmt, buf_t *out);
/* decompress one chunk, tell how many input bytes it used */
int comp_decompress_chunk(const uint8_t *in, size_t inlen, buf_t *out, size_t *used);

typedef struct {
    comp_fmt_t fmt;
    buf_t data;
} comp_chunk_t;

typedef struct {
    comp_chunk_t *items;
    size_t n, cap;
} comp_chunks_t;

int comp_split(const uint8_t *in, size_t inlen, comp_chunks_t *out);
void comp_chunks_free(comp_chunks_t *c);

/* ----------------------------------------------------------------- cpio */

typedef struct {
    char *name;
    uint32_t mode, uid, gid, nlink, mtime;
    uint32_t devmajor, devminor, rdevmajor, rdevminor, ino;
    buf_t data;
} cpio_entry_t;

typedef struct {
    cpio_entry_t *entries;
    size_t n, cap;
    char *label;
    buf_t gap;
} cpio_seg_t;

typedef struct {
    cpio_seg_t *segs;
    size_t n, cap;
} cpio_archive_t;

#define CPIO_IS_DIR(e)   (((e)->mode & 0170000) == 0040000)
#define CPIO_IS_LINK(e)  (((e)->mode & 0170000) == 0120000)
#define CPIO_PERMS(e)    ((e)->mode & 07777)

void cpio_init(cpio_archive_t *a);
void cpio_free(cpio_archive_t *a);
int cpio_parse(const uint8_t *data, size_t len, cpio_archive_t *a);
int cpio_serialize_seg(const cpio_seg_t *seg, buf_t *out);
int cpio_serialize(const cpio_archive_t *a, buf_t *out);
cpio_entry_t *cpio_seg_find(cpio_seg_t *seg, const char *name);
cpio_entry_t *cpio_find(cpio_archive_t *a, const char *name);
int cpio_add(cpio_archive_t *a, size_t seg, const cpio_entry_t *entry);
int cpio_ensure_dir(cpio_archive_t *a, size_t seg, const char *path);
int cpio_entry_set_data(cpio_entry_t *e, const void *data, size_t len);
void cpio_entry_free(cpio_entry_t *e);
size_t cpio_main_segment(cpio_archive_t *a);
int cpio_extract_dir(cpio_archive_t *a, const char *root);
int cpio_build_dir(const char *root, cpio_archive_t *a);

/* -------------------------------------------------------------- boot img */

#define VP_MAX_IMAGES 4
#define VP_ROLE_NAMES 4

typedef struct {
    uint32_t size, offset, type;
    char name[32];
    uint8_t *board_id;
    size_t board_id_len;
} vendor_fragment_t;

typedef struct {
    int is_vendor;                  /* vendor_boot.img */
    char role[16];                  /* boot / init_boot / vendor_boot ... */
    const char *path;               /* source file (not owned) */
    uint32_t header_version;
    uint32_t page_size;
    uint32_t os_version;
    char name[32];
    char cmdline[1600];             /* v3/v4 full cmdline */
    char cmdline_main[520];         /* v0-2 */
    char cmdline_extra[1040];       /* v0-2 */
    buf_t kernel, ramdisk, second, dtb, recovery_dtbo, boot_signature;
    buf_t bootconfig;
    buf_t raw_header;
    buf_t unwrapped;      /* holds a decompressed copy of the source file */
    size_t header_span;
    size_t header_offset;
    vendor_fragment_t *frags;
    size_t n_frags;
    uint32_t frag_entry_size;
    comp_fmt_t *chunk_fmts;         /* per ramdisk chunk */
    size_t n_chunks;
} boot_img_t;

void boot_img_init(boot_img_t *img);
void boot_img_free(boot_img_t *img);
int boot_img_parse(const uint8_t *data, size_t len, const char *role,
                   const char *path, boot_img_t *img);
int boot_img_pack(boot_img_t *img, buf_t *out);
const char *boot_img_cmdline(boot_img_t *img);
int boot_img_append_cmdline(boot_img_t *img, const char *extra);
int boot_img_ramdisk_archive(boot_img_t *img, cpio_archive_t *a);
int boot_img_set_ramdisk(boot_img_t *img, cpio_archive_t *a, int force);
int boot_img_has_ramdisk(boot_img_t *img);
buf_t *boot_img_dtb(boot_img_t *img);   /* may be NULL */

/* ------------------------------------------------------------- detection */

typedef struct {
    char arch[16];
    int android_api;
    char android_version[16];
    char layout[16];
    char target[16];
    int system_as_root;
    int gki;
    int already_patched;
    int n_segments;
    int has_vendor_boot;
    int recovery_fragment;
    char slot[8];
    const boot_img_t *target_img;
    const boot_img_t *boot;
    const boot_img_t *init_boot;
    const boot_img_t *vendor_boot;
} analysis_t;

typedef struct {
    const char *role;
    boot_img_t *img;
} image_set_t;

int detect_arch(const uint8_t *kernel, size_t klen, const uint8_t *dtb, size_t dlen,
                char *out, size_t outsz);
void detect_analyze(image_set_t *set, size_t n, analysis_t *res);
/* output modes for detect_print() */
typedef enum {
    VP_OUT_RICH = 0,   /* grouped, annotated report (default) */
    VP_OUT_JSON = 1,   /* machine readable */
    VP_OUT_BRIEF = 2   /* magiskboot-style KEY:VALUE */
} vp_out_mode;

void detect_print(analysis_t *res, int mode);

/* ------------------------------------------------------------------ json */

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } json_type;

typedef struct json_val json_val;

struct json_val {
    json_type type;
    double num;
    int boolean;
    char *str;                      /* J_STR */
    json_val **items;               /* J_ARR */
    char **keys;                    /* J_OBJ */
    size_t n;
};

int json_parse(const char *text, json_val **out);
void json_free(json_val *v);
json_val *json_get(json_val *obj, const char *key);
const char *json_get_str(json_val *obj, const char *key, const char *def);
double json_get_num(json_val *obj, const char *key, double def);
int json_get_bool(json_val *obj, const char *key, int def);

/* ---------------------------------------------------------------- payload */

#define VP_MAX_FILES 64
#define VP_MAX_ARCH 8
#define VP_MAX_IMPORT 8

typedef struct {
    char src[256];
    char dest[256];
    int mode;
    int uid, gid;
    char context[128];
    char backup_as[256];
    int required;
    char symlink[256];
} payload_file_t;

typedef struct {
    char file[256];
    char *import_into[VP_MAX_IMPORT];
    int n_import;
    char append_to[256];
    char *content;
} payload_rc_t;

typedef struct {
    char name[64];
    char version[32];
    char arch[VP_MAX_ARCH][16];
    int n_arch;
    int min_api, max_api;
    payload_file_t files[VP_MAX_FILES];
    int n_files;
    payload_rc_t rc;
    int has_rc;
    char cmdline_append[512];
    char selinux[16];
    char root[512];
} payload_t;

int payload_load(const char *dir, payload_t *p);
void payload_free(payload_t *p);
int payload_check(payload_t *p, const char *arch, int api, char *msg, size_t msgz);

typedef struct {
    char added[VP_MAX_FILES * 2][256];
    int n_added;
    char replaced[VP_MAX_FILES][256];
    int n_replaced;
    char backed_up[VP_MAX_FILES][256];
    int n_backed;
} inject_result_t;

int payload_apply(cpio_archive_t *a, size_t seg, payload_t *p, inject_result_t *r,
                  int selinux_permissive, const char *extra_cmdline);

/* --------------------------------------------------------------- strategy */

typedef struct {
    int patch_vendor_boot;
    int permissive;
    int force;
    int dry_run;
    int no_backup;
    int segment;                    /* -1 = auto */
    int ramdisk_format;             /* -1 = keep, else comp_fmt_t */
    const char *cmdline;
    const char *output;
} options_t;

void options_init(options_t *o);
void strategy_print_plan(analysis_t *res, payload_t *p, options_t *o, int as_json);

#endif /* VP_H */
