/*!
 * @file sofab_test_json.c
 * @brief Minimal recursive-descent JSON reader (see sofab_test_json.h).
 *
 * SPDX-License-Identifier: MIT
 */

#include "sofab_test_json.h"

#include <stdlib.h>
#include <string.h>

struct sofab_json
{
    sofab_json_type_t type;

    /* SOFAB_JSON_STRING / SOFAB_JSON_NUMBER */
    char  *text;     /* string: unescaped bytes; number: raw token; NUL-terminated */
    size_t text_len;

    /* SOFAB_JSON_BOOL */
    int    boolean;

    /* SOFAB_JSON_ARRAY / SOFAB_JSON_OBJECT */
    sofab_json_t **items;
    char         **keys;   /* object only; parallel to items */
    size_t         count;
};

typedef struct
{
    const char *p;
    const char *end;
    char       *err;
    size_t      errlen;
    int         failed;
} parser_t;

static void fail(parser_t *ps, const char *msg)
{
    if (!ps->failed && ps->err && ps->errlen)
    {
        strncpy(ps->err, msg, ps->errlen - 1);
        ps->err[ps->errlen - 1] = '\0';
    }
    ps->failed = 1;
}

static void skip_ws(parser_t *ps)
{
    while (ps->p < ps->end)
    {
        char c = *ps->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            ps->p++;
        else
            break;
    }
}

static sofab_json_t *node_new(sofab_json_type_t t)
{
    sofab_json_t *n = (sofab_json_t *)calloc(1, sizeof(*n));
    if (n) n->type = t;
    return n;
}

static sofab_json_t *parse_value(parser_t *ps);

/* Append one byte to a growing buffer. */
static int buf_push(char **buf, size_t *len, size_t *cap, char c)
{
    if (*len + 1 >= *cap)
    {
        size_t ncap = *cap ? *cap * 2 : 32;
        char  *nb   = (char *)realloc(*buf, ncap);
        if (!nb) return -1;
        *buf = nb;
        *cap = ncap;
    }
    (*buf)[(*len)++] = c;
    return 0;
}

/* Encode a Unicode code point as UTF-8 into the buffer. */
static int push_utf8(char **buf, size_t *len, size_t *cap, unsigned cp)
{
    if (cp < 0x80)
        return buf_push(buf, len, cap, (char)cp);
    if (cp < 0x800)
    {
        if (buf_push(buf, len, cap, (char)(0xC0 | (cp >> 6)))) return -1;
        return buf_push(buf, len, cap, (char)(0x80 | (cp & 0x3F)));
    }
    if (buf_push(buf, len, cap, (char)(0xE0 | (cp >> 12)))) return -1;
    if (buf_push(buf, len, cap, (char)(0x80 | ((cp >> 6) & 0x3F)))) return -1;
    return buf_push(buf, len, cap, (char)(0x80 | (cp & 0x3F)));
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse a JSON string (cursor is at the opening quote). Returns malloc'd
 * unescaped NUL-terminated bytes and sets *out_len; NULL on error. */
static char *parse_string_raw(parser_t *ps, size_t *out_len)
{
    char  *buf = NULL;
    size_t len = 0, cap = 0;

    if (ps->p >= ps->end || *ps->p != '"') { fail(ps, "expected string"); return NULL; }
    ps->p++;

    while (ps->p < ps->end)
    {
        unsigned char c = (unsigned char)*ps->p++;
        if (c == '"')
        {
            if (buf_push(&buf, &len, &cap, '\0')) { fail(ps, "oom"); free(buf); return NULL; }
            if (out_len) *out_len = len - 1;
            return buf;
        }
        if (c == '\\')
        {
            if (ps->p >= ps->end) break;
            char e = *ps->p++;
            int  rc = 0;
            switch (e)
            {
                case '"':  rc = buf_push(&buf, &len, &cap, '"');  break;
                case '\\': rc = buf_push(&buf, &len, &cap, '\\'); break;
                case '/':  rc = buf_push(&buf, &len, &cap, '/');  break;
                case 'b':  rc = buf_push(&buf, &len, &cap, '\b'); break;
                case 'f':  rc = buf_push(&buf, &len, &cap, '\f'); break;
                case 'n':  rc = buf_push(&buf, &len, &cap, '\n'); break;
                case 'r':  rc = buf_push(&buf, &len, &cap, '\r'); break;
                case 't':  rc = buf_push(&buf, &len, &cap, '\t'); break;
                case 'u':
                {
                    if (ps->end - ps->p < 4) { fail(ps, "bad \\u"); free(buf); return NULL; }
                    int h0 = hexval(ps->p[0]), h1 = hexval(ps->p[1]);
                    int h2 = hexval(ps->p[2]), h3 = hexval(ps->p[3]);
                    if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0)
                    { fail(ps, "bad \\u hex"); free(buf); return NULL; }
                    ps->p += 4;
                    rc = push_utf8(&buf, &len, &cap,
                                   (unsigned)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3));
                    break;
                }
                default: fail(ps, "bad escape"); free(buf); return NULL;
            }
            if (rc) { fail(ps, "oom"); free(buf); return NULL; }
        }
        else
        {
            if (buf_push(&buf, &len, &cap, (char)c)) { fail(ps, "oom"); free(buf); return NULL; }
        }
    }
    fail(ps, "unterminated string");
    free(buf);
    return NULL;
}

static sofab_json_t *parse_string(parser_t *ps)
{
    size_t len = 0;
    char  *s = parse_string_raw(ps, &len);
    if (!s) return NULL;
    sofab_json_t *n = node_new(SOFAB_JSON_STRING);
    if (!n) { free(s); fail(ps, "oom"); return NULL; }
    n->text = s;
    n->text_len = len;
    return n;
}

static sofab_json_t *parse_number(parser_t *ps)
{
    const char *start = ps->p;
    while (ps->p < ps->end)
    {
        char c = *ps->p;
        if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' ||
            c == 'e' || c == 'E')
            ps->p++;
        else
            break;
    }
    size_t n = (size_t)(ps->p - start);
    if (n == 0) { fail(ps, "expected number"); return NULL; }
    sofab_json_t *node = node_new(SOFAB_JSON_NUMBER);
    if (!node) { fail(ps, "oom"); return NULL; }
    node->text = (char *)malloc(n + 1);
    if (!node->text) { free(node); fail(ps, "oom"); return NULL; }
    memcpy(node->text, start, n);
    node->text[n] = '\0';
    node->text_len = n;
    return node;
}

static sofab_json_t *parse_literal(parser_t *ps)
{
    if ((size_t)(ps->end - ps->p) >= 4 && memcmp(ps->p, "true", 4) == 0)
    {
        ps->p += 4;
        sofab_json_t *n = node_new(SOFAB_JSON_BOOL);
        if (n) n->boolean = 1; else fail(ps, "oom");
        return n;
    }
    if ((size_t)(ps->end - ps->p) >= 5 && memcmp(ps->p, "false", 5) == 0)
    {
        ps->p += 5;
        sofab_json_t *n = node_new(SOFAB_JSON_BOOL);
        if (n) n->boolean = 0; else fail(ps, "oom");
        return n;
    }
    if ((size_t)(ps->end - ps->p) >= 4 && memcmp(ps->p, "null", 4) == 0)
    {
        ps->p += 4;
        sofab_json_t *n = node_new(SOFAB_JSON_NULL);
        if (!n) fail(ps, "oom");
        return n;
    }
    fail(ps, "unexpected token");
    return NULL;
}

static int container_push(sofab_json_t *c, sofab_json_t *item, char *key)
{
    sofab_json_t **ni = (sofab_json_t **)realloc(c->items, (c->count + 1) * sizeof(*ni));
    if (!ni) return -1;
    c->items = ni;
    if (key || c->keys)
    {
        char **nk = (char **)realloc(c->keys, (c->count + 1) * sizeof(*nk));
        if (!nk) return -1;
        c->keys = nk;
    }
    c->items[c->count] = item;
    if (c->keys) c->keys[c->count] = key;
    c->count++;
    return 0;
}

static sofab_json_t *parse_array(parser_t *ps)
{
    sofab_json_t *arr = node_new(SOFAB_JSON_ARRAY);
    if (!arr) { fail(ps, "oom"); return NULL; }
    ps->p++; /* [ */
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == ']') { ps->p++; return arr; }
    for (;;)
    {
        sofab_json_t *item = parse_value(ps);
        if (ps->failed) { sofab_json_free(item); sofab_json_free(arr); return NULL; }
        if (container_push(arr, item, NULL)) { fail(ps, "oom"); sofab_json_free(item); sofab_json_free(arr); return NULL; }
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',') { ps->p++; skip_ws(ps); continue; }
        if (ps->p < ps->end && *ps->p == ']') { ps->p++; break; }
        fail(ps, "expected , or ]"); sofab_json_free(arr); return NULL;
    }
    return arr;
}

static sofab_json_t *parse_object(parser_t *ps)
{
    sofab_json_t *obj = node_new(SOFAB_JSON_OBJECT);
    if (!obj) { fail(ps, "oom"); return NULL; }
    /* mark as object so keys array is allocated even when empty members appear */
    ps->p++; /* { */
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == '}') { ps->p++; return obj; }
    for (;;)
    {
        skip_ws(ps);
        size_t klen = 0;
        char  *key = parse_string_raw(ps, &klen);
        if (!key) { sofab_json_free(obj); return NULL; }
        skip_ws(ps);
        if (ps->p >= ps->end || *ps->p != ':') { fail(ps, "expected :"); free(key); sofab_json_free(obj); return NULL; }
        ps->p++;
        skip_ws(ps);
        sofab_json_t *val = parse_value(ps);
        if (ps->failed) { free(key); sofab_json_free(val); sofab_json_free(obj); return NULL; }
        if (container_push(obj, val, key)) { fail(ps, "oom"); free(key); sofab_json_free(val); sofab_json_free(obj); return NULL; }
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',') { ps->p++; continue; }
        if (ps->p < ps->end && *ps->p == '}') { ps->p++; break; }
        fail(ps, "expected , or }"); sofab_json_free(obj); return NULL;
    }
    return obj;
}

static sofab_json_t *parse_value(parser_t *ps)
{
    skip_ws(ps);
    if (ps->p >= ps->end) { fail(ps, "unexpected end"); return NULL; }
    char c = *ps->p;
    switch (c)
    {
        case '{': return parse_object(ps);
        case '[': return parse_array(ps);
        case '"': return parse_string(ps);
        case 't': case 'f': case 'n': return parse_literal(ps);
        default:
            if (c == '-' || (c >= '0' && c <= '9')) return parse_number(ps);
            fail(ps, "unexpected character");
            return NULL;
    }
}

sofab_json_t *sofab_json_parse(const char *text, size_t len, char *err, size_t errlen)
{
    parser_t ps;
    ps.p = text;
    ps.end = text + len;
    ps.err = err;
    ps.errlen = errlen;
    ps.failed = 0;
    if (err && errlen) err[0] = '\0';

    sofab_json_t *root = parse_value(&ps);
    if (ps.failed) { sofab_json_free(root); return NULL; }
    skip_ws(&ps);
    return root;
}

void sofab_json_free(sofab_json_t *v)
{
    if (!v) return;
    if (v->items)
    {
        for (size_t i = 0; i < v->count; i++)
            sofab_json_free(v->items[i]);
        free(v->items);
    }
    if (v->keys)
    {
        for (size_t i = 0; i < v->count; i++)
            free(v->keys[i]);
        free(v->keys);
    }
    free(v->text);
    free(v);
}

sofab_json_type_t sofab_json_type(const sofab_json_t *v)
{
    return v ? v->type : SOFAB_JSON_NULL;
}

const sofab_json_t *sofab_json_get(const sofab_json_t *v, const char *key)
{
    if (!v || v->type != SOFAB_JSON_OBJECT || !v->keys) return NULL;
    for (size_t i = 0; i < v->count; i++)
        if (v->keys[i] && strcmp(v->keys[i], key) == 0)
            return v->items[i];
    return NULL;
}

size_t sofab_json_array_size(const sofab_json_t *v)
{
    return (v && v->type == SOFAB_JSON_ARRAY) ? v->count : 0;
}

const sofab_json_t *sofab_json_array_at(const sofab_json_t *v, size_t i)
{
    if (!v || v->type != SOFAB_JSON_ARRAY || i >= v->count) return NULL;
    return v->items[i];
}

const char *sofab_json_string(const sofab_json_t *v, size_t *len)
{
    if (!v || v->type != SOFAB_JSON_STRING) { if (len) *len = 0; return NULL; }
    if (len) *len = v->text_len;
    return v->text;
}

int sofab_json_bool(const sofab_json_t *v)
{
    return (v && v->type == SOFAB_JSON_BOOL) ? v->boolean : 0;
}

uint64_t sofab_json_u64(const sofab_json_t *v)
{
    if (!v || v->type != SOFAB_JSON_NUMBER) return 0;
    return strtoull(v->text, NULL, 10);
}

int64_t sofab_json_i64(const sofab_json_t *v)
{
    if (!v || v->type != SOFAB_JSON_NUMBER) return 0;
    return strtoll(v->text, NULL, 10);
}

double sofab_json_double(const sofab_json_t *v)
{
    if (!v || v->type != SOFAB_JSON_NUMBER) return 0.0;
    return strtod(v->text, NULL);
}
