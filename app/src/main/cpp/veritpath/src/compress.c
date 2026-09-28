/* veritpath - Android boot image analyzer and payload injector.
 *
 * _GNU_SOURCE is defined here rather than on the command line so the sources
 * compile identically under -std=c11, -std=gnu11 and any cross toolchain:
 * glibc otherwise hides PATH_MAX, strtok_r, symlink, readlink and lstat.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

/* Compression backends.
 *
 * gzip       -> zlib (always available: glibc, musl and the Android NDK all ship libz)
 * lz4        -> implemented here (block format + legacy/frame containers)
 * xz/lzma    -> liblzma when the header is present at build time
 * bzip2      -> libbz2 when present
 * zstd       -> libzstd when present
 *
 * Everything optional is guarded so a plain `cc *.c -lz` still builds.
 */
#include "vp.h"

#include <stdlib.h>

#if defined(HAVE_LZMA)
#include <lzma.h>
#endif
#if defined(HAVE_BZIP2)
#include <bzlib.h>
#endif
#if defined(HAVE_ZSTD)
#include <zstd.h>
#endif
#include <zlib.h>

comp_fmt_t comp_detect(const uint8_t *d, size_t len)
{
    if (len >= 2 && d[0] == 0x1f && d[1] == 0x8b)
        return FMT_GZIP;
    if (len >= 6 && memcmp(d, "\xfd" "7zXZ\x00", 6) == 0)
        return FMT_XZ;
    if (len >= 4 && memcmp(d, "\x04\x22\x4d\x18", 4) == 0)
        return FMT_LZ4;
    if (len >= 4 && memcmp(d, "\x02\x21\x4c\x18", 4) == 0)
        return FMT_LZ4_LEGACY;
    if (len >= 4 && memcmp(d, "\x28\xb5\x2f\xfd", 4) == 0)
        return FMT_ZSTD;
    if (len >= 3 && memcmp(d, "BZh", 3) == 0)
        return FMT_BZIP2;
    if (len >= 3 && d[0] == 0x5d && d[1] == 0x00 && d[2] == 0x00)
        return FMT_LZMA;
    return FMT_RAW;
}

const char *comp_name(comp_fmt_t f)
{
    switch (f) {
    case FMT_GZIP: return "gzip";
    case FMT_XZ: return "xz";
    case FMT_LZMA: return "lzma";
    case FMT_LZ4: return "lz4";
    case FMT_LZ4_LEGACY: return "lz4_legacy";
    case FMT_BZIP2: return "bzip2";
    case FMT_ZSTD: return "zstd";
    default: return "raw";
    }
}

comp_fmt_t comp_from_name(const char *name)
{
    for (int i = 0; i <= (int)FMT_ZSTD; i++) {
        if (strcmp(name, comp_name((comp_fmt_t)i)) == 0)
            return (comp_fmt_t)i;
    }
    return FMT_RAW;
}

/* --------------------------------------------------------------- lz4 core */

static int lz4_block(const uint8_t *src, size_t len, buf_t *out)
{
    size_t pos = 0;
    while (pos < len) {
        uint8_t token = src[pos++];
        size_t lit = token >> 4;
        if (lit == 15) {
            for (;;) {
                if (pos >= len)
                    return -1;
                uint8_t b = src[pos++];
                lit += b;
                if (b != 255)
                    break;
            }
        }
        if (lit) {
            if (pos + lit > len)
                return -1;
            buf_append(out, src + pos, lit);
            pos += lit;
        }
        if (pos >= len)
            break;
        if (pos + 2 > len)
            return -1;
        uint32_t offset = (uint32_t)(src[pos] | (src[pos + 1] << 8));
        pos += 2;
        if (offset == 0 || offset > out->len)
            return -1;
        size_t match = token & 0x0F;
        if (match == 15) {
            for (;;) {
                if (pos >= len)
                    return -1;
                uint8_t b = src[pos++];
                match += b;
                if (b != 255)
                    break;
            }
        }
        match += 4;
        size_t start = out->len - offset;
        buf_reserve(out, match);
        for (size_t i = 0; i < match; i++)
            out->data[out->len++] = out->data[start + i];
        out->data[out->len] = 0;
    }
    return 0;
}

static int lz4_legacy_buf(const uint8_t *src, size_t len, buf_t *out, size_t *used)
{
    if (len < 4 || memcmp(src, "\x02\x21\x4c\x18", 4) != 0)
        return -1;
    size_t pos = 4;
    for (;;) {
        if (pos + 4 > len)
            return -1;
        uint32_t raw = (uint32_t)(src[pos] | (src[pos + 1] << 8) | (src[pos + 2] << 16) |
                                  ((uint32_t)src[pos + 3] << 24));
        pos += 4;
        if (raw == 0)
            break;
        int compressed = !(raw & 0x80000000u);
        size_t size = raw & 0x7FFFFFFFu;
        if (pos + size > len)
            return -1;
        if (compressed) {
            if (lz4_block(src + pos, size, out) != 0)
                return -1;
        } else {
            buf_append(out, src + pos, size);
        }
        pos += size;
    }
    if (used)
        *used = pos;
    return 0;
}

static int lz4_frame_buf(const uint8_t *src, size_t len, buf_t *out, size_t *used)
{
    if (len < 7 || memcmp(src, "\x04\x22\x4d\x18", 4) != 0)
        return -1;
    uint8_t flg = src[4], bd = src[5];
    int has_size = (flg & 0x08) != 0;
    int has_dict = (flg & 0x01) != 0;
    int has_block_crc = (flg & 0x10) != 0;
    static const uint32_t sizes[] = {4, 64, 256, 1024};
    uint32_t block_max = sizes[(bd >> 4) & 0x03] * 1024;
    size_t pos = 6;
    if (has_size)
        pos += 8;
    if (has_dict)
        pos += 4;
    pos += 1; /* header checksum */
    for (;;) {
        if (pos + 4 > len)
            return -1;
        uint32_t raw = (uint32_t)(src[pos] | (src[pos + 1] << 8) | (src[pos + 2] << 16) |
                                  ((uint32_t)src[pos + 3] << 24));
        pos += 4;
        if (raw == 0)
            break;
        size_t size = raw & 0x7FFFFFFFu;
        int uncompressed = (raw & 0x80000000u) != 0;
        if (size > block_max)
            return -1;
        if (pos + size > len)
            return -1;
        if (uncompressed)
            buf_append(out, src + pos, size);
        else if (lz4_block(src + pos, size, out) != 0)
            return -1;
        pos += size;
        if (has_block_crc)
            pos += 4;
    }
    if (flg & 0x04)
        pos += 4;
    if (used)
        *used = pos;
    return 0;
}

/* LZ4 output: legacy container with uncompressed blocks.  The legacy format
 * allows raw blocks, so every bootloader LZ4 decoder accepts this, and we stay
 * free of any external compressor. */
static int lz4_compress_buf(const uint8_t *in, size_t inlen, int legacy, buf_t *out)
{
    if (legacy) {
        uint8_t hdr[4] = {0x02, 0x21, 0x4c, 0x18};
        buf_append(out, hdr, 4);
    } else {
        uint8_t hdr[7] = {0x04, 0x22, 0x4d, 0x18, 0x40, 0x70, 0x00};
        buf_append(out, hdr, 7);
    }
    const size_t chunk = 1u << 20;
    size_t pos = 0;
    while (pos < inlen) {
        size_t n = inlen - pos;
        if (n > chunk)
            n = chunk;
        uint8_t raw[4];
        uint32_t v = legacy ? (uint32_t)(n | 0x80000000u) : (uint32_t)(n | 0x80000000u);
        raw[0] = (uint8_t)(v & 0xff);
        raw[1] = (uint8_t)((v >> 8) & 0xff);
        raw[2] = (uint8_t)((v >> 16) & 0xff);
        raw[3] = (uint8_t)((v >> 24) & 0xff);
        buf_append(out, raw, 4);
        buf_append(out, in + pos, n);
        pos += n;
    }
    uint8_t end[4] = {0, 0, 0, 0};
    buf_append(out, end, 4);
    if (!legacy) {
        uint8_t endmark[4] = {0, 0, 0, 0};
        buf_append(out, endmark, 4);
    }
    return 0;
}

/* ------------------------------------------------------------- public api */

int comp_decompress_chunk(const uint8_t *in, size_t inlen, buf_t *out, size_t *used)
{
    comp_fmt_t f = comp_detect(in, inlen);
    *used = inlen;
    switch (f) {
    case FMT_GZIP: {
        z_stream s;
        memset(&s, 0, sizeof(s));
        if (inflateInit2(&s, 15 + 32) != Z_OK)
            return -1;
        s.next_in = (Bytef *)in;
        s.avail_in = (uInt)inlen;
        uint8_t tmp[65536];
        for (;;) {
            s.next_out = tmp;
            s.avail_out = sizeof(tmp);
            int r = inflate(&s, Z_NO_FLUSH);
            buf_append(out, tmp, sizeof(tmp) - s.avail_out);
            if (r == Z_STREAM_END)
                break;
            if (r != Z_OK && r != Z_BUF_ERROR) {
                inflateEnd(&s);
                return -1;
            }
            if (r == Z_BUF_ERROR && s.avail_in == 0)
                break;
        }
        *used = inlen - s.avail_in;
        inflateEnd(&s);
        return 0;
    }
    case FMT_LZ4_LEGACY:
        return lz4_legacy_buf(in, inlen, out, used);
    case FMT_LZ4:
        return lz4_frame_buf(in, inlen, out, used);
#if defined(HAVE_LZMA)
    case FMT_XZ:
    case FMT_LZMA: {
        lzma_stream s = LZMA_STREAM_INIT;
        lzma_ret r = (f == FMT_XZ) ? lzma_stream_decoder(&s, UINT64_MAX, 0)
                                   : lzma_alone_decoder(&s, UINT64_MAX);
        if (r != LZMA_OK)
            return -1;
        s.next_in = in;
        s.avail_in = inlen;
        uint8_t tmp[65536];
        for (;;) {
            s.next_out = tmp;
            s.avail_out = sizeof(tmp);
            r = lzma_code(&s, LZMA_FINISH);
            buf_append(out, tmp, sizeof(tmp) - s.avail_out);
            if (r == LZMA_STREAM_END)
                break;
            if (r != LZMA_OK && r != LZMA_BUF_ERROR) {
                lzma_end(&s);
                return -1;
            }
        }
        *used = inlen - s.avail_in;
        lzma_end(&s);
        return 0;
    }
#endif
#if defined(HAVE_BZIP2)
    case FMT_BZIP2: {
        bz_stream s;
        memset(&s, 0, sizeof(s));
        if (BZ2_bzDecompressInit(&s, 0, 0) != BZ_OK)
            return -1;
        s.next_in = (char *)in;
        s.avail_in = (unsigned)inlen;
        char tmp[65536];
        for (;;) {
            s.next_out = tmp;
            s.avail_out = sizeof(tmp);
            int r = BZ2_bzDecompress(&s);
            buf_append(out, tmp, sizeof(tmp) - s.avail_out);
            if (r == BZ_STREAM_END)
                break;
            if (r != BZ_OK && r != BZ_PARAM_ERROR) {
                BZ2_bzDecompressEnd(&s);
                return -1;
            }
        }
        *used = inlen - s.avail_in;
        BZ2_bzDecompressEnd(&s);
        return 0;
    }
#endif
#if defined(HAVE_ZSTD)
    case FMT_ZSTD: {
        unsigned long long cap = ZSTD_getFrameContentSize(in, inlen);
        size_t guess = (cap == ZSTD_CONTENTSIZE_UNKNOWN) ? inlen * 4 + 1024 : (size_t)cap;
        buf_reserve(out, guess);
        out->len = guess;
        size_t r = ZSTD_decompress(out->data, guess, in, inlen);
        if (ZSTD_isError(r)) {
            out->len = 0;
            return -1;
        }
        out->len = r;
        return 0;
    }
#endif
    default:
        buf_append(out, in, inlen);
        return 0;
    }
}

int comp_decompress(const uint8_t *in, size_t inlen, buf_t *out)
{
    size_t used = 0;
    return comp_decompress_chunk(in, inlen, out, &used);
}

int comp_compress(const uint8_t *in, size_t inlen, comp_fmt_t fmt, buf_t *out)
{
    switch (fmt) {
    case FMT_GZIP: {
        z_stream s;
        memset(&s, 0, sizeof(s));
        if (deflateInit2(&s, 9, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
            return -1;
        buf_reserve(out, deflateBound(&s, (uLong)inlen) + 32);
        s.next_in = (Bytef *)in;
        s.avail_in = (uInt)inlen;
        for (;;) {
            buf_reserve(out, 65536);
            s.next_out = out->data + out->len;
            s.avail_out = (uInt)(out->cap - out->len - 1);
            int r = deflate(&s, Z_FINISH);
            out->len = out->cap - s.avail_out - 1;
            if (r == Z_STREAM_END)
                break;
            if (r != Z_OK && r != Z_BUF_ERROR) {
                deflateEnd(&s);
                return -1;
            }
        }
        out->data[out->len] = 0;
        deflateEnd(&s);
        return 0;
    }
    case FMT_LZ4:
    case FMT_LZ4_LEGACY:
        return lz4_compress_buf(in, inlen, fmt == FMT_LZ4_LEGACY, out);
#if defined(HAVE_LZMA)
    case FMT_XZ:
    case FMT_LZMA: {
        lzma_stream s = LZMA_STREAM_INIT;
        lzma_ret r = (fmt == FMT_XZ)
                         ? lzma_easy_encoder(&s, 9 | LZMA_PRESET_EXTREME, LZMA_CHECK_CRC64)
                         : lzma_alone_encoder(&s, NULL);
        if (r != LZMA_OK)
            return -1;
        s.next_in = in;
        s.avail_in = inlen;
        for (;;) {
            buf_reserve(out, 65536);
            s.next_out = out->data + out->len;
            s.avail_out = out->cap - out->len - 1;
            r = lzma_code(&s, LZMA_FINISH);
            out->len = out->cap - s.avail_out - 1;
            if (r == LZMA_STREAM_END)
                break;
            if (r != LZMA_OK && r != LZMA_BUF_ERROR) {
                lzma_end(&s);
                return -1;
            }
        }
        out->data[out->len] = 0;
        lzma_end(&s);
        return 0;
    }
#endif
#if defined(HAVE_BZIP2)
    case FMT_BZIP2: {
        bz_stream s;
        memset(&s, 0, sizeof(s));
        if (BZ2_bzCompressInit(&s, 9, 0, 30) != BZ_OK)
            return -1;
        s.next_in = (char *)in;
        s.avail_in = (unsigned)inlen;
        for (;;) {
            buf_reserve(out, 65536);
            s.next_out = (char *)(out->data + out->len);
            s.avail_out = (unsigned)(out->cap - out->len - 1);
            int r = BZ2_bzCompress(&s, BZ_FINISH);
            out->len = out->cap - s.avail_out - 1;
            if (r == BZ_STREAM_END)
                break;
            if (r != BZ_OK && r != BZ_PARAM_ERROR) {
                BZ2_bzCompressEnd(&s);
                return -1;
            }
        }
        out->data[out->len] = 0;
        BZ2_bzCompressEnd(&s);
        return 0;
    }
#endif
    default:
        buf_append(out, in, inlen);
        return 0;
    }
}

void comp_chunks_free(comp_chunks_t *c)
{
    for (size_t i = 0; i < c->n; i++)
        buf_free(&c->items[i].data);
    free(c->items);
    c->items = NULL;
    c->n = c->cap = 0;
}

int comp_split(const uint8_t *in, size_t inlen, comp_chunks_t *out)
{
    memset(out, 0, sizeof(*out));
    size_t pos = 0;
    while (pos < inlen) {
        while (pos < inlen && in[pos] == 0)
            pos++;
        if (pos >= inlen)
            break;
        buf_t data;
        buf_init(&data);
        size_t used = 0;
        if (comp_decompress_chunk(in + pos, inlen - pos, &data, &used) != 0) {
            buf_free(&data);
            return -1;
        }
        if (used == 0)
            used = inlen - pos;
        if (out->n == out->cap) {
            out->cap = out->cap ? out->cap * 2 : 4;
            out->items = xrealloc(out->items, out->cap * sizeof(*out->items));
        }
        out->items[out->n].fmt = comp_detect(in + pos, inlen - pos);
        out->items[out->n].data = data;
        out->n++;
        pos += used;
    }
    if (out->n == 0) {
        buf_t data;
        buf_init(&data);
        buf_append(&data, in, inlen);
        out->items = xmalloc(sizeof(*out->items));
        out->items[0].fmt = FMT_RAW;
        out->items[0].data = data;
        out->n = out->cap = 1;
    }
    return 0;
}
