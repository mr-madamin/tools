/* The brain: read the local folder, ask the peer for its manifest, diff the
   two, and decide every PUT and DELETE. --dry-run prints the plan and sends
   nothing; --delete turns "the peer has extras" into actual removals. */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "framing.h"
#include "json.h"
#include "util.h"

/* Without these the program blocks in the kernel with nothing on screen: a
   dropped SYN takes ~75s to fail on macOS, and a peer that accepts and then
   says nothing waits forever. Same three phases as sync_push.py. */
#define CONNECT_TIMEOUT  8   /* TCP handshake */
#define PROTOCOL_TIMEOUT 15  /* HELLO / MANIFEST reply */
#define TRANSFER_TIMEOUT 300 /* any single stalled send during the transfer */

static const char *USAGE =
    "usage: sync_push [peer_host] [root_dir] [--delete | --dry-run]";

/* The TCP connection is up but the peer never answered — a different failure
   from "can't get there", and it points at the other end, not the network. */
static const char *silent_peer_help(const char *host, int port, const char *stage)
{
    static char msg[512];
    snprintf(msg, sizeof(msg),
             "Connected to %s:%d, but the peer never answered %s within %ds.\n\n"
             "The TCP connection is up, so this is the other end misbehaving:\n"
             "  - Is something ELSE listening on that port (not sync_server)?\n"
             "  - Is the server wedged in an earlier session? Restart it.",
             host, port, stage, PROTOCOL_TIMEOUT);
    return msg;
}

/* Python's hint(): a warning that isn't fatal, set off from the normal log. */
static void hint(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("  ! ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

/* connect() has no timeout of its own, and the macOS default is ~75s of silence
   against a dropped SYN. Go non-blocking, poll for the deadline, then hand back
   a blocking socket so the rest of the code stays straight-line. */
static int connect_timeout(struct addrinfo *ai, int seconds)
{
    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0)
        return -1;

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        return -1;
    }

    if (connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
        if (errno != EINPROGRESS) {
            int saved = errno;
            close(fd);
            errno = saved;
            return -1;
        }

        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLOUT;
        int n;
        do {
            n = poll(&pfd, 1, seconds * 1000);
        } while (n < 0 && errno == EINTR);

        if (n == 0) { /* the SYN went out and nothing came back at all */
            close(fd);
            errno = ETIMEDOUT;
            return -1;
        }
        if (n < 0) {
            int saved = errno;
            close(fd);
            errno = saved;
            return -1;
        }

        /* poll() says "done", not "succeeded" — a refusal wakes it too. */
        int err = 0;
        socklen_t elen = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 || err != 0) {
            close(fd);
            errno = err != 0 ? err : EIO;
            return -1;
        }
    }

    fcntl(fd, F_SETFL, flags); /* blocking again; SO_RCVTIMEO bounds it now */
    return fd;
}

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
        die("can't resolve peer host '%s': %s", host, gai_strerror(err));

    printf("Connecting (timeout %ds) ...\n", CONNECT_TIMEOUT);

    int fd = -1, last = 0;
    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        fd = connect_timeout(ai, CONNECT_TIMEOUT);
        if (fd >= 0)
            break;
        last = errno;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        if (last == ETIMEDOUT)
            die("No answer from %s:%d after %ds.\n\n"
                "The connection request went out and nothing came back -- not even\n"
                "a refusal. Something is DROPPING the packet:\n"
                "  1. Firewall on the receiver (most common). Stealth mode drops\n"
                "     silently, exactly like this.\n"
                "  2. Client isolation on the router (guest network / AP isolation).\n"
                "  3. Wrong or stale IP -- DHCP moved the receiver.\n"
                "  4. The server isn't running at all.\n\n"
                "Narrow it down from HERE:\n"
                "  ping -c 3 %s      # no replies -> network/firewall, not the port\n"
                "  nc -vz %s %d      # 'succeeded' -> port is open, rerun the push\n\n"
                "The server prints 'Connected from ...' the instant a peer arrives.\n"
                "If it stays quiet while this side waits, the packet never reached\n"
                "it -- look at the firewall and the router, not at this program.",
                host, port, CONNECT_TIMEOUT, host, host, port);
        if (last == ECONNREFUSED)
            die("Connection refused by %s:%d.\n\n"
                "Good news: the host is reachable and answered. Nothing is listening\n"
                "on that port for that address.\n\n"
                "  - Is the server running on the receiver?\n"
                "  - Was it started with --lan? Without it the server binds\n"
                "    127.0.0.1 only, and refuses connections on the LAN IP.\n"
                "  - Do the ports match? This side is using %d (config.json\n"
                "    peer.port).",
                host, port, port);
        die("Can't reach %s:%d -- %s.\n\n"
            "The route doesn't exist: this Mac has no path to that address.\n"
            "  - Is this Mac on Wi-Fi at all?  ipconfig getifaddr en0\n"
            "  - Same network as the receiver, and the same subnet?\n"
            "  - A VPN can take the LAN route away; turn it off and retry.",
            host, port, strerror(last));
    }
    return fd;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);

    int dry_run = 0, delete_extras = 0;
    const char *positional[2] = { NULL, NULL };
    size_t npos = 0;
    strlist unknown;
    sl_init(&unknown);

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--", 2) == 0) {
            if (strcmp(argv[i], "--dry-run") == 0)
                dry_run = 1;
            else if (strcmp(argv[i], "--delete") == 0)
                delete_extras = 1;
            else if (strcmp(argv[i], "--help") == 0) {
                printf("%s\n\n%s\n", USAGE,
                       "  --dry-run   print the plan (puts and deletes), send nothing\n"
                       "  --delete    also remove files the peer has and the source\n"
                       "              doesn't, making the peer an exact mirror\n"
                       "  --help      this message\n\n"
                       "peer_host defaults to config.json peer.host, root_dir to\n"
                       "shared_dir. The port always comes from config.json peer.port.");
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

    config *cfg = load_config(argv[0]);

    const char *host = positional[0] != NULL ? positional[0]
                       : cfg->peer_host != NULL ? cfg->peer_host
                                                : "127.0.0.1";
    int port = cfg->peer_port != 0 ? cfg->peer_port : 8765;
    const char *root_dir = positional[1] != NULL ? positional[1]
                           : cfg->shared_dir != NULL ? cfg->shared_dir
                                                     : "sandbox/source";

    printf("Push %s  ->  %s:%d\n", root_dir, host, port);

    /* Both of these end in an empty local manifest, which with --delete reads as
       "the source has nothing, so remove everything" — check before we connect. */
    if (root_dir[0] == '~')
        die("root_dir '%s' starts with '~', which is NOT expanded here --\n"
            "it would walk an empty folder and push nothing. Use an absolute path.",
            root_dir);

    struct stat root_st;
    if (stat(root_dir, &root_st) != 0 || !S_ISDIR(root_st.st_mode)) {
        char cwd[PATH_MAX];
        die("root_dir does not exist: %s\n"
            "(cwd: %s) -- check the path, or mkdir -p it.",
            path_abs(root_dir), getcwd(cwd, sizeof(cwd)) != NULL ? cwd : "?");
    }

    int sock = dial(host, port);

    /* Connected. From here a wedged peer is the risk, not an unreachable one. */
    sock_set_timeout(sock, PROTOCOL_TIMEOUT);

    json_value *reply = NULL;
    int rc = handshake(sock, cfg->token, &reply);
    if (rc == FRAME_TIMEOUT)
        die("%s", silent_peer_help(host, port, "HELLO"));
    if (rc != FRAME_OK)
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
    rc = recv_msg(sock, &payload, &len); /* the manifest: MAX_FRAME, not control */
    if (rc == FRAME_TIMEOUT)
        die("%s", silent_peer_help(host, port, "MANIFEST"));
    if (rc == FRAME_TOOBIG)
        die("peer's manifest frame exceeds %u bytes -- refusing to allocate it.\n"
            "That's not a folder listing; check what's actually on %s:%d.",
            MAX_FRAME, host, port);
    if (rc != FRAME_OK)
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

    /* An empty source is almost always a wrong path, not a real "delete
       everything" — refuse to mirror it. --dry-run still shows the plan. */
    if (local.count == 0) {
        char *abs = path_abs(root_dir);
        hint("no files found under %s", abs);
        free(abs);
        hint("is that the right folder? nothing will be sent");
        if (delete_extras && !dry_run)
            die("refusing to run --delete from an empty source: it would wipe the peer.");
    }

    /* A big file legitimately takes a while; only a fully stalled send is a
       failure, so the transfer phase gets a much longer leash. */
    sock_set_timeout(sock, TRANSFER_TIMEOUT);

    printf("%s %zu file(s):\n", dry_run ? "Would send" : "Sending", to_put.count);
    for (size_t i = 0; i < to_put.count; i++) {
        printf("   PUT    %s\n", to_put.items[i]);
        if (dry_run)
            continue;
        int put = send_file(sock, root_dir, to_put.items[i]);
        if (put == FRAME_TIMEOUT)
            die("stalled for %ds while sending '%s'.\n"
                "The peer stopped reading -- server killed, or the Wi-Fi dropped.",
                TRANSFER_TIMEOUT, to_put.items[i]);
        if (put != 0)
            die("peer went away while sending '%s' (%s).\n"
                "Check the server's terminal for the error it printed.",
                to_put.items[i], strerror(errno));
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
