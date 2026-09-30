#ifndef FS_NET_H
#define FS_NET_H

#include <stddef.h>
#include <sys/socket.h>

// "host:port", "[v6]:port" or just "host" (default port). 
int net_split_addr(const char *s, char *host, size_t hn, char *port, size_t pn);

int  net_listen(const char *hostport);                    // fd or -1 
int  net_connect(const char *hostport, int timeout_ms);   // fd or -1 
void net_set_timeouts(int fd, int sec);
void net_addr_str(const struct sockaddr *sa, socklen_t len, char *out, size_t n);

#endif
