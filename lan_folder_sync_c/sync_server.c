/* The passive side: bind, accept, authenticate, then do exactly what the peer
   asks — serve a manifest, accept a PUT, remove a DELETE. It never decides
   anything about the sync; the pusher is the brain. */
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "config.h"
#include "framing.h"
#include "json.h"
#include "util.h"

static const char *USAGE = "usage: sync_server [shared_dir] [port] [--lan]";

/* The server handles one session at a time, so a peer that connects and then
   says nothing isn't just its own problem — it locks out everyone else until
   this process is killed. Bound the wait. Generous, because a legitimate peer
   can be busy walking a large folder between frames. Overridable at build time
   so the wedge can actually be tested without waiting five minutes. */
#ifndef SESSION_TIMEOUT
#define SESSION_TIMEOUT 300
#endif

/* This Mac's en0 IPv4 (Wi-Fi, usually), or NULL if offline / not on en0.
   Python shells out to `ipconfig getifaddr en0`; getifaddrs() is the same
   answer without a subprocess. */
static char *lan_ip(void)
{
    struct ifaddrs *ifaddr;
    if (getifaddrs(&ifaddr) != 0)
        return NULL;

    char *found = NULL;
    for (struct ifaddrs *ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET)
            continue;
        if (strcmp(ifa->ifa_name, "en0") != 0)
            continue;
        if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK))
            continue;

        char buf[INET_ADDRSTRLEN];
        struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
        if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf)) != NULL) {
            found = xstrdup(buf);
            break;
        }
    }
    freeifaddrs(ifaddr);
    return found;
}

/* Python prints the op with !r, so a string shows as 'MANFEST' and a missing
   one as None. Match that, so the two servers' errors read identically. */
static char *op_repr(const json_value *header)
{
    const json_value *op = json_get(header, "op");
    if (op == NULL || op->type == JSON_NULL)
        return xstrdup("None");
    if (op->type != JSON_STRING)
        return xstrdup("<non-string>");

    strbuf sb;
    sb_init(&sb);
    sb_addch(&sb, '\'');
    sb_addstr(&sb, op->string);
    sb_addch(&sb, '\'');
    return sb_detach(&sb, NULL);
}

static void serve_session(int conn, const char *shared_dir, const char *token);

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN); /* a vanished peer is an error code, not a death */

    config *cfg = load_config(argv[0]);

    int want_lan = 0;
    const char *positional[2] = { NULL, NULL };
    size_t npos = 0;
    strlist unknown;
    sl_init(&unknown);

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--", 2) == 0) {
            if (strcmp(argv[i], "--lan") == 0)
                want_lan = 1;
            else if (strcmp(argv[i], "--help") == 0) {
                printf("%s\n\n%s\n", USAGE,
                       "  --lan    bind this Mac's en0 IPv4 instead of 127.0.0.1, so\n"
                       "           peers on the LAN can reach it. Exits if en0 has no\n"
                       "           address rather than quietly serving loopback.\n"
                       "  --help   this message\n\n"
                       "shared_dir defaults to config.json shared_dir, port to\n"
                       "peer.port. Without --lan the server binds 127.0.0.1.");
                return 0;
            } else
                sl_push(&unknown, xstrdup(argv[i]));
        } else if (npos < 2) {
            positional[npos++] = argv[i];
        }
    }
    if (unknown.count > 0)
        die_unknown_flags(&unknown, USAGE);
    sl_free(&unknown);

    const char *shared_dir = positional[0] != NULL ? positional[0]
                             : cfg->shared_dir  != NULL ? cfg->shared_dir
                                                        : "sandbox/received";
    int port = positional[1] != NULL ? atoi(positional[1])
               : cfg->peer_port != 0 ? cfg->peer_port
                                     : 8765;

    char *bind_host = NULL;
    if (want_lan) {
        bind_host = lan_ip();
        if (bind_host == NULL)
            die("--lan: no en0 IPv4 found (offline, or Wi-Fi isn't en0). "
                "Join the LAN, or drop --lan to bind 127.0.0.1");
    } else {
        bind_host = xstrdup("127.0.0.1");
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        die("socket: %s", strerror(errno));

    int one = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, bind_host, &addr.sin_addr) != 1)
        die("bad bind address: %s", bind_host);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0)
        die("bind %s:%d: %s", bind_host, port, strerror(errno));
    if (listen(sock, 1) != 0)
        die("listen: %s", strerror(errno));

    printf("Serving %s/ on %s:%d ... (Ctrl-C to stop)\n", shared_dir, bind_host, port);
    if (strcmp(bind_host, "127.0.0.1") != 0)
        printf("  \xe2\x86\x92 peers: set config.json peer.host to '%s'\n", bind_host);

    for (;;) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        int conn = accept(sock, (struct sockaddr *)&peer, &peer_len);
        if (conn < 0) {
            if (errno == EINTR)
                continue;
            die("accept: %s", strerror(errno));
        }

        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        printf("Connected from %s:%d\n", ip, ntohs(peer.sin_port));

        sock_set_timeout(conn, SESSION_TIMEOUT);
        serve_session(conn, shared_dir, cfg->token);
        close(conn);
        printf("Session ended, waiting for next peer\n");
    }
}

static void serve_session(int conn, const char *shared_dir, const char *token)
{
    int authenticated = 0;

    for (;;) {
        char *payload = NULL;
        size_t len = 0;
        /* Everything a client sends us is a short header; the big frame in this
           protocol only ever travels the other way. */
        int rc = recv_msg_max(conn, &payload, &len, MAX_CONTROL_FRAME);
        if (rc != FRAME_OK) {
            if (rc == FRAME_TOOBIG) {
                printf("Refused oversized frame (cap %u bytes)\n", MAX_CONTROL_FRAME);
                send_error(conn, "frame too large");
            } else if (rc == FRAME_TIMEOUT)
                printf("Session timed out: peer idle for %ds, dropping it\n",
                       SESSION_TIMEOUT);
            else if (rc == FRAME_ERR)
                printf("Session error: connection lost\n");
            break; /* peer vanished, or went quiet and stayed quiet */
        }

        json_value *header = NULL;
        if (is_valid_utf8(payload, len))
            header = json_parse(payload, len);
        free(payload);

        if (header == NULL || header->type != JSON_OBJECT) {
            json_free(header);
            send_error(conn, "malformed header: not valid UTF-8 JSON");
            break;
        }

        const char *op = json_get_str(header, "op");

        if (!authenticated) {
            if (op == NULL || strcmp(op, "HELLO") != 0) {
                send_error(conn, "auth required: send HELLO first");
                json_free(header);
                break;
            }
            const char *got = json_get_str(header, "token");
            if (got == NULL || strcmp(got, token) != 0) {
                send_error(conn, "bad token");
                json_free(header);
                break;
            }
            authenticated = 1;
            send_json(conn, "{\"op\": \"OK\"}");
            printf("     HELLO ok, session authenticated\n");
            json_free(header);
            continue;
        }

        if (op != NULL && strcmp(op, "MANIFEST") == 0) {
            manifest m;
            build_manifest(shared_dir, &m);
            char *files = manifest_to_json(&m);

            strbuf sb;
            sb_init(&sb);
            sb_addstr(&sb, "{\"op\": \"MANIFEST\", \"files\": ");
            sb_addstr(&sb, files);
            sb_addch(&sb, '}');
            send_msg(conn, sb.data, sb.len);
            sb_free(&sb);
            free(files);

            printf("Sent manifest (%zu files)\n", m.count);
            manifest_free(&m);
        } else if (op != NULL && strcmp(op, "PUT") == 0) {
            char *rel_path = NULL;
            const char *missing = NULL;
            int body = recv_file_body(conn, shared_dir, header, &rel_path, &missing);

            if (body == BODY_MISSING) {
                char msg[128];
                snprintf(msg, sizeof(msg), "PUT header missing field: '%s'", missing);
                send_error(conn, msg);
                json_free(header);
                break; /* body size unknown — can't resync the stream */
            }
            if (body == BODY_UNSAFE) {
                const char *bad = json_get_str(header, "path");
                strbuf sb;
                sb_init(&sb);
                sb_addf(&sb, "unsafe path refused: '%s'", bad != NULL ? bad : "");
                send_error(conn, sb.data);
                sb_free(&sb);
                json_free(header);
                break;
            }
            if (body == BODY_TIMEOUT) {
                printf("Session timed out: peer stalled mid-file after %ds\n",
                       SESSION_TIMEOUT);
                json_free(header);
                break;
            }
            if (body == BODY_IO) {
                printf("Session error: peer closed mid-file - truncated transfer\n");
                json_free(header);
                break;
            }
            printf("    received %s\n", rel_path);
            free(rel_path);
        } else if (op != NULL && strcmp(op, "BYE") == 0) {
            printf("Peer said BYE\n");
            json_free(header);
            break;
        } else if (op != NULL && strcmp(op, "DELETE") == 0) {
            const char *rel_path = json_get_str(header, "path");
            if (rel_path == NULL) {
                send_error(conn, "DELETE missing 'path'");
                json_free(header);
                break;
            }

            char *target = path_is_lexically_safe(rel_path)
                               ? safe_path(shared_dir, rel_path)
                               : NULL;
            if (target == NULL) {
                strbuf sb;
                sb_init(&sb);
                sb_addf(&sb, "unsafe path refused: '%s'", rel_path);
                send_error(conn, sb.data);
                sb_free(&sb);
                json_free(header);
                break;
            }

            if (unlink(target) == 0)
                printf("    deleted %s\n", rel_path);
            else
                printf("    already gone %s\n", rel_path); /* delete is idempotent */
            free(target);
        } else {
            char *repr = op_repr(header);
            strbuf sb;
            sb_init(&sb);
            sb_addf(&sb, "unknown op: %s", repr);
            send_error(conn, sb.data);
            sb_free(&sb);
            free(repr);
            json_free(header);
            break; /* unknown frame */
        }

        json_free(header);
    }
}
