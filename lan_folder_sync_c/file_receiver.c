/* Scratch demo, kept for parity with file_receiver.py: accept one file, then
   quit. The real server is sync_server. */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "framing.h"
#include "util.h"

int main(void)
{
    const int port = 8765;
    const char *dest = "received";
    mkdir_p(dest);

    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0)
        die("socket: %s", strerror(errno));

    int one = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) != 0)
        die("bind: %s", strerror(errno));
    listen(server, 1);
    printf("Listening on %d ... waiting for a file\n", port);

    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    int conn = accept(server, (struct sockaddr *)&peer, &peer_len);
    if (conn < 0)
        die("accept: %s", strerror(errno));

    char ip[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
    printf("Connected from %s:%d\n", ip, ntohs(peer.sin_port));

    char *path = NULL;
    if (recv_file(conn, dest, &path) != FRAME_OK)
        die("receive failed");
    printf("Received -> %s\n", path);

    free(path);
    close(conn);
    close(server);
    return 0;
}
