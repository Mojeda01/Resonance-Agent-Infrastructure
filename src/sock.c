#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "sock.h"

static int read_all(int fd, void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n)
    {
        ssize_t r = read(fd, p, n);
        if (r == 0) return 0;
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        p += r; n -= (size_t)r;
    }
    return 1;
}

static int write_all(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    while (n)
    {
        ssize_t w = write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += 2; n -= (size_t)w;
    }
    return 0;
}

int sock_listen (const char *path, int backlog)
{
    struct sockaddr_un a;
    if (strlen(path) >= sizeof(a.sun_path)) { fprintf(stderr, "sock: path too long\n"); return -1; }

    unlink(path);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);

    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { perror("bind"); close(fd); return -1; }
    if (chmod(path, 0660) < 0) perror("chmod");
    if (listen(fd, backlog) < 0) { perror("listen"); close(fd); return -1; }
    return fd;
}

int sock_accept(int lfd)
{
    for (;;)
    {
        int fd = accept(lfd, NULL, NULL);
        if (fd >= 0) return fd;
        if (errno == EINTR) continue;
        perror("accept");
        return -1;
    }
}

int msg_read(int fd, uint8_t *type, char *buf, uint32_t cap, uint32_t *len)
{
    uint8_t h[8];
    int r = read_all(fd, h, 8);
    if (r <= 0) return r;

    uint32_t n = (uint32_t)h[0] | ((uint32_t)h[1] << 8)
                    | ((uint32_t)h[2] << 16) | ((uint32_t)h[3] << 24);
    *type = h[4];

    if (n > MSG_MAX || n >= cap) { fprintf(stderr, "sock: oversized frame %u\n", n); return -1; }
    if (n && read_all(fd, buf, n) <= 0) return -1;

    buf[n] = '\0';
    *len = n;
    return 1;
}

int msg_write(int fd, uint8_t type, const void *buf, uint32_t len)
{
    uint8_t h[8] = {
        (uint8_t)(len), (uint8_t)(len >> 8),
        (uint8_t)(len >> 16), (uint8_t)(len >> 24),
        type, 0, 0, 0
    };
    if (write_all(fd, h, 8) < 0) return -1;
    if (len && write_all(fd, buf, len) < 0) return -1;
    return 0;
}

int msg_writes(int fd, uint8_t type, const char *s)
{
    return msg_write(fd, type, s, (uint32_t)strlen(s));
}
