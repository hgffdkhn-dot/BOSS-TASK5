/* veritpath - POSIX portability shims.
 *
 * Included at the top of every source file (before anything else that might
 * pull in system headers).
 *
 * Two problems are handled here:
 *
 * 1. _GNU_SOURCE is meaningless on MinGW. It is what makes glibc expose
 *    strtok_r, symlink, readlink, lstat, PATH_MAX and friends, but the Windows
 *    headers have no such switch - they simply do not provide several POSIX
 *    functions at all. So on Windows we do not rely on it; we declare or
 *    substitute what is missing.
 *
 * 2. Some functions exist with a different signature. mkdir() on MinGW takes
 *    only a path, so mkdir(p, 0755) is a hard compile error, not a warning.
 */
#ifndef VP_COMPAT_H
#define VP_COMPAT_H

#if !defined(_WIN32) && !defined(_WIN64)
#  ifndef _GNU_SOURCE
#    define _GNU_SOURCE 1
#  endif
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#  include <direct.h>     /* _mkdir */
#  include <io.h>
#  ifndef _O_BINARY
#    define _O_BINARY 0
#  endif
/* MinGW spells it _fileno; glibc spells it fileno */
#  ifndef _fileno
#    define _fileno(f) fileno(f)
#  endif

/* mkdir takes one argument here */
#  ifndef VP_HAVE_MKDIR_1
#    define VP_HAVE_MKDIR_1 1
#  endif
#  ifdef mkdir
#    undef mkdir
#  endif
#  define mkdir(path, mode) _mkdir(path)

/* no symbolic links in a practical sense; treat nothing as a link */
#  ifndef S_ISLNK
#    define S_ISLNK(m) (0)
#  endif
#  ifndef S_IFLNK
#    define S_IFLNK 0xA000
#  endif

/* POSIX functions MinGW omits */
static inline char *vp_realpath(const char *path, char *out)
{
    return _fullpath(out, path, 260 /* _MAX_PATH */);
}
#  ifndef realpath
#    define realpath(p, o) vp_realpath((p), (o))
#  endif

static inline void *vp_memmem_shim(const void *hay, size_t haylen,
                                   const void *needle, size_t needlelen)
{
    const unsigned char *h = (const unsigned char *)hay;
    const unsigned char *n = (const unsigned char *)needle;
    if (!needlelen)
        return (void *)h;
    if (haylen < needlelen)
        return NULL;
    for (size_t i = 0; i + needlelen <= haylen; i++) {
        if (h[i] == n[0] && memcmp(h + i, n, needlelen) == 0)
            return (void *)(h + i);
    }
    return NULL;
}
#  ifndef memmem
#    define memmem(h, hl, n, nl) vp_memmem_shim((h), (hl), (n), (nl))
#  endif

#else  /* POSIX */
#  include <unistd.h>
#  include <limits.h>
#  ifndef PATH_MAX
#    define PATH_MAX 4096
#  endif
#endif

#endif /* VP_COMPAT_H */
