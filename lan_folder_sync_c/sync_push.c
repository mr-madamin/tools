/* The brain: read the local folder, ask the peer for its manifest, diff the
   two, and decide every PUT and DELETE. --dry-run prints the plan and sends
   nothing; --delete turns "the peer has extras" into actual removals. */
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "config.h"
#include "framing.h"
#include "json.h"
#include "util.h"

static int dial(const char *host, int port)
{
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    int err = getaddrinfo(host, portstr, &hints, &res);
    if (err != 0)
        die("cannot resolve %s: %s", host, gai_strerror(err));

    int fd = -1;
    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0)
        die("cannot connect to %s:%d: %s", host, port, strerror(errno));
    return fd;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);

    int dry_run = 0, delete_extras = 0;
    const char *positional[2] = { NULL, NULL };
    size_t npos = 0;

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--", 2) == 0) {
            if (strcmp(argv[i], "--dry-run") == 0)
                dry_run = 1;
            else if (strcmp(argv[i], "--delete") == 0)
                delete_extras = 1;
        } else if (npos < 2) {
            positional[npos++] = argv[i];
        }
    }

    config *cfg = load_config(argv[0]);

    const char *host = positional[0] != NULL ? positional[0]
                       : cfg->peer_host != NULL ? cfg->peer_host
                                                : "127.0.0.1";
    int port = cfg->peer_port != 0 ? cfg->peer_port : 8765;
    const char *root_dir = positional[1] != NULL ? positional[1]
                           : cfg->shared_dir != NULL ? cfg->shared_dir
                                                     : "sandbox/source";

    int sock = dial(host, port);

    json_value *reply = NULL;
    if (handshake(sock, cfg->token, &reply) != FRAME_OK)
        die("peer closed during handshake (wrong token?)");

    const char *op = json_get_str(reply, "op");
    if (op == NULL || strcmp(op, "OK") != 0) {
        const char *message = json_get_str(reply, "message");
        die("handshake refused: %s", message != NULL ? message : "None");
    }
    json_free(reply);

    manifest local;
    build_manifest(root_dir, &local);

    send_json(sock, "{\"op\": \"MANIFEST\"}");

    char *payload = NULL;
    size_t len = 0;
    if (recv_msg(sock, &payload, &len) != FRAME_OK)
        die("Connection closed by peer before manifest was received");

    json_value *msg = json_parse(payload, len);
    free(payload);
    if (msg == NULL)
        die("peer sent a manifest we can't parse");

    manifest remote;
    if (manifest_from_json(json_get(msg, "files"), &remote) != 0)
        die("peer's manifest has no \"files\": %s", json_get_str(msg, "message"));

    strlist to_put, to_delete;
    sl_init(&to_put);
    sl_init(&to_delete);
    diff_manifests(&local, &remote, MTIME_TOLERANCE, &to_put, &to_delete);

    if (dry_run)
        printf("DRY RUN - no files will be sent\n");
    printf("Local: %zu files | Peer: %zu files\n", local.count, remote.count);

    printf("%s %zu file(s):\n", dry_run ? "Would send" : "Sending", to_put.count);
    for (size_t i = 0; i < to_put.count; i++) {
        printf("   PUT    %s\n", to_put.items[i]);
        if (!dry_run && send_file(sock, root_dir, to_put.items[i]) != 0)
            die("failed to send %s: %s", to_put.items[i], strerror(errno));
    }

    if (to_delete.count > 0) {
        const char *verb = dry_run          ? "Would delete"
                           : delete_extras  ? "Deleting"
                                            : "Skipping (need --delete):";
        printf("%s %zu file(s):\n", verb, to_delete.count);
        for (size_t i = 0; i < to_delete.count; i++) {
            printf("   DELETE %s\n", to_delete.items[i]);
            if (!dry_run && delete_extras)
                send_delete(sock, to_delete.items[i]);
        }
    }

    send_json(sock, "{\"op\": \"BYE\"}");
    close(sock);

    sl_free(&to_put);
    sl_free(&to_delete);
    manifest_free(&local);
    manifest_free(&remote);
    json_free(msg);
    config_free(cfg);
    return 0;
}
