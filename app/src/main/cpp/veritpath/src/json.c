/* veritpath - Android boot image analyzer and payload injector.
 *
 * _GNU_SOURCE is defined here rather than on the command line so the sources
 * compile identically under -std=c11, -std=gnu11 and any cross toolchain:
 * glibc otherwise hides PATH_MAX, strtok_r, symlink, readlink and lstat.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

/* Minimal JSON reader - just enough for veritpath manifests. */
#include "vp.h"

#include <ctype.h>
#include <stdlib.h>

typedef struct {
    const char *p;
    const char *end;
    int depth;
} jparse_t;

static json_val *jv_new(json_type t)
{
    json_val *v = xmalloc(sizeof(json_val));
    memset(v, 0, sizeof(*v));
    v->type = t;
    return v;
}

static void skip_ws(jparse_t *s)
{
    while (s->p < s->end && (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' || *s->p == '\r'))
        s->p++;
}

static json_val *parse_value(jparse_t *s);

static char *parse_string(jparse_t *s)
{
    if (s->p >= s->end || *s->p != '"')
        return NULL;
    s->p++;
    buf_t b;
    buf_init(&b);
    while (s->p < s->end && *s->p != '"') {
        char c = *s->p++;
        if (c == '\\' && s->p < s->end) {
            char e = *s->p++;
            switch (e) {
            case 'n': buf_append(&b, "\n", 1); break;
            case 't': buf_append(&b, "\t", 1); break;
            case 'r': buf_append(&b, "\r", 1); break;
            case 'b': buf_append(&b, "\b", 1); break;
            case 'f': buf_append(&b, "\f", 1); break;
            case '"': buf_append(&b, "\"", 1); break;
            case '\\': buf_append(&b, "\\", 1); break;
            case '/': buf_append(&b, "/", 1); break;
            case 'u': {
                unsigned cp = 0;
                for (int i = 0; i < 4 && s->p < s->end; i++) {
                    char h = *s->p++;
                    cp <<= 4;
                    if (h >= '0' && h <= '9')
                        cp |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f')
                        cp |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F')
                        cp |= (unsigned)(h - 'A' + 10);
                }
                /* ASCII only; anything else becomes '?' - manifests are ASCII */
                buf_append(&b, cp < 128 ? (const char *)&cp : "?", 1);
                break;
            }
            default: buf_append(&b, &e, 1); break;
            }
        } else {
            buf_append(&b, &c, 1);
        }
    }
    if (s->p < s->end)
        s->p++; /* closing quote */
    char *out = xmalloc(b.len + 1);
    if (b.len)
        memcpy(out, b.data, b.len);
    out[b.len] = 0;
    buf_free(&b);
    return out;
}

static json_val *parse_value(jparse_t *s)
{
    skip_ws(s);
    if (s->p >= s->end)
        return NULL;
    char c = *s->p;
    if (c == '{') {
        s->p++;
        json_val *v = jv_new(J_OBJ);
        skip_ws(s);
        if (s->p < s->end && *s->p == '}') {
            s->p++;
            return v;
        }
        for (;;) {
            skip_ws(s);
            char *k = parse_string(s);
            if (!k)
                break;
            skip_ws(s);
            if (s->p < s->end && *s->p == ':')
                s->p++;
            json_val *val = parse_value(s);
            v->keys = xrealloc(v->keys, (v->n + 1) * sizeof(char *));
            v->items = xrealloc(v->items, (v->n + 1) * sizeof(json_val *));
            v->keys[v->n] = k;
            v->items[v->n] = val;
            v->n++;
            skip_ws(s);
            if (s->p < s->end && *s->p == ',') {
                s->p++;
                continue;
            }
            if (s->p < s->end && *s->p == '}')
                s->p++;
            break;
        }
        return v;
    }
    if (c == '[') {
        s->p++;
        json_val *v = jv_new(J_ARR);
        skip_ws(s);
        if (s->p < s->end && *s->p == ']') {
            s->p++;
            return v;
        }
        for (;;) {
            json_val *item = parse_value(s);
            v->items = xrealloc(v->items, (v->n + 1) * sizeof(json_val *));
            v->items[v->n++] = item;
            skip_ws(s);
            if (s->p < s->end && *s->p == ',') {
                s->p++;
                continue;
            }
            if (s->p < s->end && *s->p == ']')
                s->p++;
            break;
        }
        return v;
    }
    if (c == '"') {
        json_val *v = jv_new(J_STR);
        v->str = parse_string(s);
        return v;
    }
    if ((c >= '0' && c <= '9') || c == '-' || c == '+') {
        json_val *v = jv_new(J_NUM);
        v->num = strtod(s->p, (char **)&s->p);
        return v;
    }
    if (strncmp(s->p, "true", 4) == 0) {
        s->p += 4;
        json_val *v = jv_new(J_BOOL);
        v->boolean = 1;
        return v;
    }
    if (strncmp(s->p, "false", 5) == 0) {
        s->p += 5;
        return jv_new(J_BOOL);
    }
    if (strncmp(s->p, "null", 4) == 0) {
        s->p += 4;
        return jv_new(J_NULL);
    }
    return NULL;
}

int json_parse(const char *text, json_val **out)
{
    jparse_t s;
    s.p = text;
    s.end = text + strlen(text);
    s.depth = 0;
    json_val *v = parse_value(&s);
    if (!v)
        return -1;
    *out = v;
    return 0;
}

void json_free(json_val *v)
{
    if (!v)
        return;
    if (v->type == J_STR)
        free(v->str);
    for (size_t i = 0; i < v->n; i++) {
        json_free(v->items[i]);
        if (v->type == J_OBJ)
            free(v->keys[i]);
    }
    free(v->items);
    free(v->keys);
    free(v);
}

json_val *json_get(json_val *obj, const char *key)
{
    if (!obj || obj->type != J_OBJ)
        return NULL;
    for (size_t i = 0; i < obj->n; i++) {
        if (strcmp(obj->keys[i], key) == 0)
            return obj->items[i];
    }
    return NULL;
}

const char *json_get_str(json_val *obj, const char *key, const char *def)
{
    json_val *v = json_get(obj, key);
    if (v && v->type == J_STR && v->str)
        return v->str;
    return def;
}

double json_get_num(json_val *obj, const char *key, double def)
{
    json_val *v = json_get(obj, key);
    if (v && v->type == J_NUM)
        return v->num;
    return def;
}

int json_get_bool(json_val *obj, const char *key, int def)
{
    json_val *v = json_get(obj, key);
    if (v && v->type == J_BOOL)
        return v->boolean;
    return def;
}
