#include "json.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *p;
    const char *end;
} parser;

static json_value *parse_value(parser *ps);

static json_value *new_value(json_type type)
{
    json_value *v = xmalloc(sizeof(*v));
    memset(v, 0, sizeof(*v));
    v->type = type;
    return v;
}

void json_free(json_value *v)
{
    if (v == NULL)
        return;
    free(v->string);
    for (size_t i = 0; i < v->count; i++) {
        if (v->keys != NULL)
            free(v->keys[i]);
        json_free(v->items[i]);
    }
    free(v->keys);
    free(v->items);
    free(v);
}

static void skip_ws(parser *ps)
{
    while (ps->p < ps->end &&
           (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
        ps->p++;
}

static int peek(parser *ps) { return ps->p < ps->end ? (unsigned char)*ps->p : -1; }

static int literal(parser *ps, const char *word)
{
    size_t n = strlen(word);
    if ((size_t)(ps->end - ps->p) < n || memcmp(ps->p, word, n) != 0)
        return 0;
    ps->p += n;
    return 1;
}

/* Encode one code point as UTF-8, the way Python's json decoder would. */
static void emit_utf8(strbuf *sb, unsigned int cp)
{
    if (cp < 0x80) {
        sb_addch(sb, (char)cp);
    } else if (cp < 0x800) {
        sb_addch(sb, (char)(0xC0 | (cp >> 6)));
        sb_addch(sb, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        sb_addch(sb, (char)(0xE0 | (cp >> 12)));
        sb_addch(sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sb_addch(sb, (char)(0x80 | (cp & 0x3F)));
    } else {
        sb_addch(sb, (char)(0xF0 | (cp >> 18)));
        sb_addch(sb, (char)(0x80 | ((cp >> 12) & 0x3F)));
        sb_addch(sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sb_addch(sb, (char)(0x80 | (cp & 0x3F)));
    }
}

static int hex4(parser *ps, unsigned int *out)
{
    if (ps->end - ps->p < 4)
        return 0;
    unsigned int value = 0;
    for (int i = 0; i < 4; i++) {
        char c = ps->p[i];
        value <<= 4;
        if (c >= '0' && c <= '9') value |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') value |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') value |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    ps->p += 4;
    *out = value;
    return 1;
}

/* Parse a quoted string body into a fresh malloc'd C string, or NULL. */
static char *parse_string_raw(parser *ps)
{
    if (peek(ps) != '"')
        return NULL;
    ps->p++;

    strbuf sb;
    sb_init(&sb);

    while (ps->p < ps->end) {
        unsigned char c = (unsigned char)*ps->p++;

        if (c == '"')
            return sb_detach(&sb, NULL);

        if (c != '\\') {
            if (c < 0x20)
                break; /* raw control character — not legal in a JSON string */
            sb_addch(&sb, (char)c);
            continue;
        }

        if (ps->p >= ps->end)
            break;
        char esc = *ps->p++;
        switch (esc) {
        case '"':  sb_addch(&sb, '"'); break;
        case '\\': sb_addch(&sb, '\\'); break;
        case '/':  sb_addch(&sb, '/'); break;
        case 'b':  sb_addch(&sb, '\b'); break;
        case 'f':  sb_addch(&sb, '\f'); break;
        case 'n':  sb_addch(&sb, '\n'); break;
        case 'r':  sb_addch(&sb, '\r'); break;
        case 't':  sb_addch(&sb, '\t'); break;
        case 'u': {
            unsigned int cp;
            if (!hex4(ps, &cp))
                goto fail;
            if (cp >= 0xD800 && cp <= 0xDBFF) { /* high surrogate: expect its pair */
                unsigned int low;
                if (ps->end - ps->p >= 2 && ps->p[0] == '\\' && ps->p[1] == 'u') {
                    ps->p += 2;
                    if (!hex4(ps, &low))
                        goto fail;
                    if (low >= 0xDC00 && low <= 0xDFFF)
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    else
                        goto fail;
                } else {
                    goto fail;
                }
            }
            emit_utf8(&sb, cp);
            break;
        }
        default:
            goto fail;
        }
    }

fail:
    sb_free(&sb);
    return NULL;
}

static void push_item(json_value *v, char *key, json_value *item)
{
    size_t cap = v->count + 1;
    v->items = xrealloc(v->items, cap * sizeof(*v->items));
    v->items[v->count] = item;
    if (v->type == JSON_OBJECT) {
        v->keys = xrealloc(v->keys, cap * sizeof(*v->keys));
        v->keys[v->count] = key;
    }
    v->count++;
}

static json_value *parse_object(parser *ps)
{
    json_value *obj = new_value(JSON_OBJECT);
    ps->p++; /* '{' */
    skip_ws(ps);

    if (peek(ps) == '}') { ps->p++; return obj; }

    for (;;) {
        skip_ws(ps);
        char *key = parse_string_raw(ps);
        if (key == NULL)
            goto fail;
        skip_ws(ps);
        if (peek(ps) != ':') { free(key); goto fail; }
        ps->p++;
        json_value *item = parse_value(ps);
        if (item == NULL) { free(key); goto fail; }
        push_item(obj, key, item);

        skip_ws(ps);
        int c = peek(ps);
        if (c == ',') { ps->p++; continue; }
        if (c == '}') { ps->p++; return obj; }
        goto fail;
    }

fail:
    json_free(obj);
    return NULL;
}

static json_value *parse_array(parser *ps)
{
    json_value *arr = new_value(JSON_ARRAY);
    ps->p++; /* '[' */
    skip_ws(ps);

    if (peek(ps) == ']') { ps->p++; return arr; }

    for (;;) {
        json_value *item = parse_value(ps);
        if (item == NULL)
            goto fail;
        push_item(arr, NULL, item);

        skip_ws(ps);
        int c = peek(ps);
        if (c == ',') { ps->p++; continue; }
        if (c == ']') { ps->p++; return arr; }
        goto fail;
    }

fail:
    json_free(arr);
    return NULL;
}

static json_value *parse_number(parser *ps)
{
    char buf[64];
    size_t n = 0;
    const char *start = ps->p;

    if (peek(ps) == '-')
        ps->p++;
    while (ps->p < ps->end && ((*ps->p >= '0' && *ps->p <= '9') || *ps->p == '.' ||
                               *ps->p == 'e' || *ps->p == 'E' || *ps->p == '+' ||
                               *ps->p == '-'))
        ps->p++;

    n = (size_t)(ps->p - start);
    if (n == 0 || n >= sizeof(buf))
        return NULL;
    memcpy(buf, start, n);
    buf[n] = '\0';

    char *tail = NULL;
    double d = strtod(buf, &tail);
    if (tail == buf || *tail != '\0')
        return NULL;

    json_value *v = new_value(JSON_NUMBER);
    v->number = d;
    return v;
}

static json_value *parse_value(parser *ps)
{
    skip_ws(ps);
    int c = peek(ps);

    switch (c) {
    case '{': return parse_object(ps);
    case '[': return parse_array(ps);
    case '"': {
        char *s = parse_string_raw(ps);
        if (s == NULL)
            return NULL;
        json_value *v = new_value(JSON_STRING);
        v->string = s;
        return v;
    }
    case 't':
        if (!literal(ps, "true")) return NULL;
        { json_value *v = new_value(JSON_BOOL); v->boolean = 1; return v; }
    case 'f':
        if (!literal(ps, "false")) return NULL;
        { json_value *v = new_value(JSON_BOOL); v->boolean = 0; return v; }
    case 'n':
        if (!literal(ps, "null")) return NULL;
        return new_value(JSON_NULL);
    default:
        if (c == '-' || (c >= '0' && c <= '9'))
            return parse_number(ps);
        return NULL;
    }
}

json_value *json_parse(const char *text, size_t len)
{
    parser ps = { text, text + len };
    json_value *v = parse_value(&ps);
    if (v == NULL)
        return NULL;

    skip_ws(&ps);
    if (ps.p != ps.end) { /* trailing garbage — Python's json.loads rejects it too */
        json_free(v);
        return NULL;
    }
    return v;
}

const json_value *json_get(const json_value *obj, const char *key)
{
    if (obj == NULL || obj->type != JSON_OBJECT)
        return NULL;
    for (size_t i = 0; i < obj->count; i++)
        if (strcmp(obj->keys[i], key) == 0)
            return obj->items[i];
    return NULL;
}

const char *json_str(const json_value *v)
{
    return (v != NULL && v->type == JSON_STRING) ? v->string : NULL;
}

const char *json_get_str(const json_value *obj, const char *key)
{
    return json_str(json_get(obj, key));
}

int json_get_num(const json_value *obj, const char *key, double *out)
{
    const json_value *v = json_get(obj, key);
    if (v == NULL || v->type != JSON_NUMBER)
        return 0;
    *out = v->number;
    return 1;
}

void json_escape(strbuf *sb, const char *s)
{
    sb_addch(sb, '"');
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
        switch (*p) {
        case '"':  sb_addstr(sb, "\\\""); break;
        case '\\': sb_addstr(sb, "\\\\"); break;
        case '\b': sb_addstr(sb, "\\b"); break;
        case '\f': sb_addstr(sb, "\\f"); break;
        case '\n': sb_addstr(sb, "\\n"); break;
        case '\r': sb_addstr(sb, "\\r"); break;
        case '\t': sb_addstr(sb, "\\t"); break;
        default:
            if (*p < 0x20)
                sb_addf(sb, "\\u%04x", *p);
            else
                sb_addch(sb, (char)*p); /* UTF-8 passes through verbatim */
        }
    }
    sb_addch(sb, '"');
}
