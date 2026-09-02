#include "framing.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define CHUNK 65536

/* ---- byte plumbing -------------------------------------------------------- */

/* Read exactly n bytes, or report that the peer closed first. The loop is the
   whole point: one recv() can return short, and regularly does. */
int recv_exactly(int fd, void *buf, size_t n)
{
    unsigned char *out = buf;
    size_t got = 0;

    while (got < n)
    {
        ssize_t r = recv(fd, out + got, n - got, 0);
        if (r == 0)
            return FRAME_EOF;
        if (r < 0)
        {
            if (errno == EINTR)
                continue;
            return FRAME_ERR;
        }
        got += (size_t)r;
    }
    return FRAME_OK;
}

int send_all(int fd, const void *buf, size_t n)
{
    const unsigned char *p = buf;
    size_t sent = 0;

    while (sent < n)
    {
        ssize_t w = send(fd, p + sent, n - sent, 0);
        if (w < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        sent += (size_t)w;
    }
    return 0;
}
