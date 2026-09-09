/* Scratch demo, kept for parity with file_sender.py: push one file, then quit.
   The real client is sync_push. */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "framing.h"
#include "util.h"

int main(int argc, char **argv)
{
    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    const char *path = argc > 2 ? argv[2] : "bigfile.bin";
    const int port = 8765;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        die("socket: %s", strerror(errno));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1)
        die("bad address: %s", host);
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0)
        die("connect: %s", strerror(errno));

    if (send_file(sock, ".", path) != 0)
        die("send_file %s: %s", path, strerror(errno));
    printf("Sent %s\n", path);

    close(sock);
    return 0;
}
