/* A minimal JSON reader/writer — the one thing Python handed us as a single
   `import json`. Enough of the spec to speak this protocol: objects, arrays,
   strings (with \u escapes and surrogate pairs), numbers, true/false/null. */
#ifndef JSON_H
#define JSON_H

#include <stddef.h>

#include "util.h"

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} json_type;

typedef struct json_value json_value;

struct json_value {
    json_type type;
    int boolean;         /* JSON_BOOL */
    double number;       /* JSON_NUMBER */
    char *string;        /* JSON_STRING, decoded UTF-8 */
    json_value **items;  /* JSON_ARRAY items, or JSON_OBJECT values */
    char **keys;         /* JSON_OBJECT keys, parallel to items */
    size_t count;
};

/* Returns NULL if the text isn't valid JSON (or has trailing garbage). */
json_value *json_parse(const char *text, size_t len);
void json_free(json_value *v);

const json_value *json_get(const json_value *obj, const char *key);
const char *json_str(const json_value *v);
const char *json_get_str(const json_value *obj, const char *key);
int json_get_num(const json_value *obj, const char *key, double *out);

/* Append `s` to sb as a quoted, escaped JSON string. */
void json_escape(strbuf *sb, const char *s);

#endif
