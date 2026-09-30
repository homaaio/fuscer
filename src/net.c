#define _GNU_SOURCE
#include "net.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "common.h"

int net_split_addr(const char *s, char *host, size_t hn, char *port, size_t pn)
{
    if (!s || !*s) return -1;

    if (s[0] == '[') {
        const char *rb = strchr(s, ']');
        if (!rb) return -1;
        size_t l = (size_t)(rb - s - 1);
        if (l >= hn) return -1;
        memcpy(host, s + 1, l);
        host[l] = '\0';
        if (rb[1] == ':') fs_copy(port, pn, rb + 2);
        else              fs_copy(port, pn, FS_DEFAULT_PORT);
        return 0;
    }

    const char *c = strrchr(s, ':');
    if (!c || strchr(s, ':') != c) { // no colon, or bare IPv6 
        fs_copy(host, hn, s);
        fs_copy(port, pn, FS_DEFAULT_PORT);
        return 0;
    }
    size_t l = (size_t)(c - s);
    if (l >= hn) return -1;
    memcpy(host, s, l);
    host[l] = '\0';
    fs_copy(port, pn, c[1] ? c + 1 : FS_DEFAULT_PORT);
    return 0;
}

void net_set_timeouts(int fd, int sec)
{
    struct timeval tv = { .tv_sec = sec, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

void net_addr_str(const struct sockaddr *sa, socklen_t len, char *out, size_t n)
{
    char h[NI_MAXHOST], p[NI_MAXSERV];
    if (getnameinfo(sa, len, h, sizeof h, p, sizeof p,
                    NI_NUMERICHOST | NI_NUMERICSERV) == 0)
        snprintf(out, n, "%s:%s", h, p);
    else
        fs_copy(out, n, "?");
}

int net_listen(const char *hostport)
{
    char host[128], port[16];
    if (net_split_addr(hostport, host, sizeof host, port, sizeof port) < 0) {
        LOGE("bad listen address: %s", hostport);
        return -1;
    }

    struct addrinfo hints, *res, *ai;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    int gai = getaddrinfo(host[0] ? host : NULL, port, &hints, &res);
    if (gai) {
        LOGE("resolve %s: %s", hostport, gai_strerror(gai));
        return -1;
    }

    int fd = -1;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 && listen(fd, 64) == 0)
            break;
        LOGE("bind/listen %s: %s", hostport, strerror(errno));
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

int net_connect(const char *hostport, int timeout_ms)
{
    char host[128], port[16];
    if (net_split_addr(hostport, host, sizeof host, port, sizeof port) < 0)
        return -1;

    struct addrinfo hints, *res, *ai;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port, &hints, &res) != 0) return -1;

    int fd = -1;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;

        int fl = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);

        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc < 0 && errno == EINPROGRESS) {
            struct pollfd pfd = { .fd = fd, .events = POLLOUT };
            if (poll(&pfd, 1, timeout_ms) == 1) {
                int err = 0;
                socklen_t el = sizeof err;
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
                rc = err ? -1 : 0;
            } else {
                rc = -1;
            }
        }
        if (rc == 0) {
            fcntl(fd, F_SETFL, fl);
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}
