#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "fuscer.h"
#include "net.h"
#include "proto.h"

static void fmt_duration(long s, char *out, size_t n)
{
    if (s < 60)         snprintf(out, n, "%lds", s);
    else if (s < 3600)  snprintf(out, n, "%ldm%02lds", s / 60, s % 60);
    else if (s < 86400) snprintf(out, n, "%ldh%02ldm", s / 3600, (s % 3600) / 60);
    else                snprintf(out, n, "%ldd%02ldh", s / 86400, (s % 86400) / 3600);
}

int status_run(const fs_config *cfg)
{
    const uint8_t *key = (const uint8_t *)cfg->key;
    size_t klen = strlen(cfg->key);

    int fd = net_connect(cfg->coordinator, 5000);
    if (fd < 0) {
        fprintf(stderr, "fuscer: cannot connect to coordinator at %s\n", cfg->coordinator);
        return 1;
    }
    net_set_timeouts(fd, 10);

    int rc = fs_send(fd, key, klen, MSG_STATUS_REQ, "", 0);
    int type = 0;
    char *body = NULL;
    size_t blen = 0;
    if (rc == FS_OK) rc = fs_recv(fd, key, klen, &type, &body, &blen);
    close(fd);

    if (rc == FS_ERR_CLOSED)
        fprintf(stderr, "fuscer: coordinator closed the connection (wrong key?)\n");
    else if (rc != FS_OK)
        fprintf(stderr, "fuscer: %s\n", fs_strerror(rc));
    if (rc != FS_OK) return 1;

    if (type != MSG_STATUS_RESP) {
        char reason[128] = "unexpected reply";
        kv_get(body, "reason", reason, sizeof reason);
        fprintf(stderr, "fuscer: %s\n", reason);
        free(body);
        return 1;
    }

    long n = kv_get_long(body, "nodes", 0);
    long up = kv_get_long(body, "uptime", 0);
    char ver[32] = "?", upstr[32];
    kv_get(body, "version", ver, sizeof ver);
    fmt_duration(up, upstr, sizeof upstr);

    int online = 0;
    for (long i = 0; i < n; i++) {
        char k[32], st[16] = "";
        snprintf(k, sizeof k, "n%ld.state", i);
        kv_get(body, k, st, sizeof st);
        if (!strcmp(st, "online")) online++;
    }

    printf("coordinator %s at %s, up %s; %ld node(s), %d online\n",
           ver, cfg->coordinator, upstr, n, online);
    if (n == 0) {
        free(body);
        return 0;
    }

    printf("\n%-16s %-8s %-14s %5s %7s %5s %-7s %-9s %s\n",
           "NODE", "STATE", "OS/ARCH", "CORES", "RAM", "LOAD", "GPU", "SEEN", "TOOLS");

    for (long i = 0; i < n; i++) {
        char name[64] = "", state[16] = "", os[32] = "", arch[32] = "";
        char gpu[16] = "", tools[256] = "";
        char k[32];

#define GET(field, buf) \
        do { snprintf(k, sizeof k, "n%ld." field, i); kv_get(body, k, buf, sizeof buf); } while (0)
#define GETL(field, def) \
        (snprintf(k, sizeof k, "n%ld." field, i), kv_get_long(body, k, def))
        GET("name", name); GET("state", state); GET("os", os);
        GET("arch", arch); GET("gpu", gpu); GET("tools", tools);

        long cores = GETL("cores", 0);
        long ram_mb = GETL("ram_mb", 0);
        long age = GETL("age", 0);
        snprintf(k, sizeof k, "n%ld.load", i);
        double load = kv_get_double(body, k, 0.0);
#undef GET
#undef GETL

        char osarch[80], ram[16], ago[32];
        snprintf(osarch, sizeof osarch, "%s/%s", os, arch);
        snprintf(ram, sizeof ram, "%.1fG", (double)ram_mb / 1024.0);
        fmt_duration(age, ago, sizeof ago);
        strncat(ago, " ago", sizeof ago - strlen(ago) - 1);

        printf("%-16s %-8s %-14s %5ld %7s %5.2f %-7s %-9s %s\n",
               name, state, osarch, cores, ram, load, gpu, ago,
               tools[0] ? tools : "-");
    }

    free(body);
    return 0;
}
