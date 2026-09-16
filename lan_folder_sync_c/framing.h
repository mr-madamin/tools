/* Wire protocol: a 4-byte big-endian length prefix, then a UTF-8 JSON payload.
   A PUT frame is followed by exactly `size` raw body bytes. Byte-for-byte the
   same protocol framing.py speaks, so a C peer and a Python peer interoperate. */
#ifndef FRAMING_H
#define FRAMING_H

#include <stddef.h>

#include "json.h"
#include "util.h"

/* Read/receive results: 1 = got it, 0 = peer closed (EOF), -1 = I/O error,
   -2 = the socket timeout expired with the peer neither sending nor closing.
   A timeout is worth its own code: "the peer is wedged" and "the peer hung up"
   need very different advice. */
#define FRAME_OK      1
#define FRAME_EOF     0
#define FRAME_ERR    (-1)
#define FRAME_TIMEOUT (-2)
#define FRAME_TOOBIG  (-3) /* peer declared a frame bigger than we'll allocate */

/* The length prefix is 4 bytes, so a peer can claim up to 4 GB before sending a
   single byte of payload — and the old code malloc'd it on the spot, before any
   authentication. Two ceilings, because the two directions carry very different
   frames: a MANIFEST lists every file in the folder, while everything else is a
   short header. The server only ever reads the short kind. */
#define MAX_FRAME         (64u * 1024 * 1024) /* a manifest of a huge folder */
#define MAX_CONTROL_FRAME (1u * 1024 * 1024)  /* HELLO / PUT / DELETE / BYE */

/* Bound every later blocking recv/send on this socket. Python spells the whole
   of this sock.settimeout(); 0 seconds clears it. */
int sock_set_timeout(int fd, int seconds);

int recv_exactly(int fd, void *buf, size_t n);
int send_all(int fd, const void *buf, size_t n); /* 0, -1, or FRAME_TIMEOUT */

int send_msg(int fd, const void *payload, size_t len);
int send_json(int fd, const char *json); /* send_msg over a NUL-terminated string */
int send_error(int fd, const char *message);

/* On FRAME_OK the caller owns *out (NUL-terminated; *out_len excludes it).
   FRAME_TOOBIG leaves the payload unread, so the stream has no boundary left to
   resync on — every caller must end the session. */
int recv_msg(int fd, char **out, size_t *out_len); /* capped at MAX_FRAME */
int recv_msg_max(int fd, char **out, size_t *out_len, size_t max);

/* 0 on success, -1 / FRAME_TIMEOUT on failure, or SEND_CHANGED when the file
   changed size while we were reading it. SEND_CHANGED is a warning, not an
   error: exactly `size` body bytes still went out, so the stream is intact and
   the session can continue — only that one file's content is suspect. */
#define SEND_CHANGED 1

int send_file(int fd, const char *root_dir, const char *rel_path);
int send_delete(int fd, const char *rel_path);

/* recv_file_body result codes. */
#define BODY_OK        0
#define BODY_MISSING (-1) /* header lacked path/size/mtime — *missing names it */
#define BODY_IO      (-2) /* peer closed mid-file, or a local write failed */
#define BODY_UNSAFE  (-3) /* path would escape dest_dir */
#define BODY_TIMEOUT (-4) /* peer went quiet mid-file without closing */
#define BODY_BADNUM  (-5) /* size/mtime present, but not a usable number */

/* size and mtime arrive as JSON doubles and were cast straight to long long /
   time_t. A double outside the target's range makes that cast undefined —
   "size": 1e999 is a one-line frame that UBSan flags outright — and a negative
   size silently truncated the destination to zero while reporting success.
   Bound both before anything is cast, opened or written. */
#define MAX_FILE_SIZE (64LL * 1024 * 1024 * 1024) /* 64 GiB per file */
#define MAX_MTIME     1e15                        /* far past any real clock */

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
    /* Symlinked directories are deliberately not followed (os.walk doesn't
       either), but skipping them in silence means a user who symlinks a folder
       into their sync directory sees nothing sync and is told nothing. Record
       them so the caller can say so. */
    strlist skipped_links;
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
