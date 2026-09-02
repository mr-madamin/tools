/* Wire protocol: a 4-byte big-endian length prefix, then a UTF-8 JSON payload.
   A PUT frame is followed by exactly `size` raw body bytes. Byte-for-byte the
   same protocol framing.py speaks, so a C peer and a Python peer interoperate. */
#ifndef FRAMING_H
#define FRAMING_H

#include <stddef.h>

#include "json.h"
#include "util.h"

/* Read/receive results: 1 = got it, 0 = peer closed (EOF), -1 = I/O error. */
#define FRAME_OK  1
#define FRAME_EOF 0
#define FRAME_ERR (-1)

int recv_exactly(int fd, void *buf, size_t n);
int send_all(int fd, const void *buf, size_t n);

int send_msg(int fd, const void *payload, size_t len);
int send_json(int fd, const char *json); /* send_msg over a NUL-terminated string */
int send_error(int fd, const char *message);

/* On FRAME_OK the caller owns *out (NUL-terminated; *out_len excludes it). */
int recv_msg(int fd, char **out, size_t *out_len);

int send_file(int fd, const char *root_dir, const char *rel_path);
int send_delete(int fd, const char *rel_path);

/* recv_file_body result codes. */
#define BODY_OK        0
#define BODY_MISSING (-1) /* header lacked path/size/mtime — *missing names it */
#define BODY_IO      (-2) /* peer closed mid-file, or a local write failed */
#define BODY_UNSAFE  (-3) /* path would escape dest_dir */

int recv_file_body(int fd, const char *dest_dir, const json_value *header,
                   char **rel_out, const char **missing);
int recv_file(int fd, const char *dest_dir, char **rel_out);

/* Client side of HELLO. Returns FRAME_OK with *reply owned by the caller. */
int handshake(int fd, const char *token, json_value **reply);

/* ---- manifests ------------------------------------------------------------ */

typedef struct {
    char *path; /* relative to the manifest root */
    long long size;
    double mtime;
} manifest_entry;

typedef struct {
    manifest_entry *items;
    size_t count;
    size_t cap;
} manifest;

void manifest_init(manifest *m);
void manifest_free(manifest *m);

int build_manifest(const char *root_dir, manifest *m);
char *manifest_to_json(const manifest *m);              /* the {"files": ...} value */
int manifest_from_json(const json_value *files, manifest *m);

#define MTIME_TOLERANCE 2.0

void diff_manifests(const manifest *local, const manifest *remote, double tolerance,
                    strlist *to_put, strlist *to_delete);

/* Reject absolute paths and any ".." component, without touching the disk. */
int path_is_lexically_safe(const char *rel_path);

/* Resolve rel_path under base and refuse anything that escapes it (symlinks
   included). Returns a malloc'd absolute path, or NULL. */
char *safe_path(const char *base, const char *rel_path);

#endif
