#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "json.h"
#include "util.h"

static char *read_whole_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;

    strbuf sb;
    sb_init(&sb);
    char chunk[8192];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
        sb_add(&sb, chunk, n);
    fclose(f);
    return sb_detach(&sb, len);
}

/* Python resolves config.json next to config.py. A C binary usually lives in
   bin/, so try the obvious places instead of guessing one. */
static char *find_config(const char *argv0)
{
    const char *env = getenv("LAN_FOLDER_SYNC_CONFIG");
    if (env != NULL && env[0] != '\0')
        return xstrdup(env);

    if (access("config.json", R_OK) == 0)
        return xstrdup("config.json");

    if (argv0 != NULL) {
        char *dir = path_dirname(argv0);
        char *here = path_join(dir, "config.json");
        if (access(here, R_OK) == 0) {
            free(dir);
            return here;
        }
        free(here);
        char *up = path_join(dir, "../config.json");
        free(dir);
        if (access(up, R_OK) == 0)
            return up;
        free(up);
    }
    return NULL;
}

/* json_get_str() returns NULL both when a key is absent and when it's present
   but isn't a string, so every mistyped value used to fall through to a default
   in silence: "port": "9999" (quoted by mistake) simply became 8765, and the
   user got "connection refused" with nothing pointing at config.json. Tell the
   two apart — absent is fine, wrong type is a typo worth naming. */
static const char *cfg_str(const json_value *obj, const char *key,
                           const char *path, const char *where)
{
    const json_value *v = json_get(obj, key);
    if (v == NULL || v->type == JSON_NULL)
        return NULL; /* genuinely absent — the caller decides if that's ok */
    if (v->type != JSON_STRING)
        die("%s: \"%s\" must be a string, in double quotes", path, where);
    return v->string;
}

config *load_config(const char *argv0)
{
    char *path = find_config(argv0);
    size_t len = 0;
    char *text = path != NULL ? read_whole_file(path, &len) : NULL;

    if (text == NULL) {
        free(path);
        die("config.json not found. Copy config.example.json to config.json and set "
            "shared_dir, peer host/port, and a shared token (same on both machines).");
    }

    json_value *root = json_parse(text, len);
    free(text);
    if (root == NULL || root->type != JSON_OBJECT)
        die("%s is not valid JSON", path);

    config *cfg = xmalloc(sizeof(*cfg));
    memset(cfg, 0, sizeof(*cfg));

    const char *s = cfg_str(root, "shared_dir", path, "shared_dir");
    if (s != NULL)
        cfg->shared_dir = xstrdup(s);

    s = cfg_str(root, "token", path, "token");
    if (s == NULL)
        die("%s: missing \"token\"", path);
    cfg->token = xstrdup(s);

    const json_value *peer = json_get(root, "peer");
    if (peer != NULL && peer->type != JSON_NULL) {
        if (peer->type != JSON_OBJECT)
            die("%s: \"peer\" must be an object, like "
                "{\"host\": \"192.168.1.42\", \"port\": 8765}",
                path);

        s = cfg_str(peer, "host", path, "peer.host");
        if (s != NULL)
            cfg->peer_host = xstrdup(s);

        const json_value *port = json_get(peer, "port");
        if (port != NULL && port->type != JSON_NULL) {
            if (port->type != JSON_NUMBER)
                die("%s: peer.port must be a number, not in quotes", path);
            /* Same range check as the command line; a JSON double also has to
               survive the cast to int, so reject inf/NaN before it. */
            if (!(port->number >= 1 && port->number <= 65535))
                die("%s: peer.port must be 1-65535", path);
            cfg->peer_port = (int)port->number;
        }
    }

    json_free(root);
    free(path);
    return cfg;
}

void config_free(config *cfg)
{
    if (cfg == NULL)
        return;
    free(cfg->shared_dir);
    free(cfg->peer_host);
    free(cfg->token);
    free(cfg);
}
