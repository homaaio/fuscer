#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "common.h"
#include "fuscer.h"
#include "net.h"
#include "proto.h"

#define MAX_NODES 128
#define MAX_CONNS 256
#define MIN_NODE_TIMEOUT 15   // seconds without a heartbeat => offline 

typedef struct {
    int      used, online;
    unsigned gen;                 // identifies the current connection 
    char     name[64], os[32], kernel[64], distro[96], arch[32];
    char     gpu[16], tools[256], addr[64];
    int      cores;
    long     ram_mb;
    double   load;
    time_t   connected, last_seen;
} node_t;

static node_t          g_nodes[MAX_NODES];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned        g_gen;
static time_t          g_start;
static atomic_int      g_conns;
static uint8_t         g_key[256];
static size_t          g_klen;

typedef struct {
    int  fd;
    char peer[64];
} conn_t;

// ---------- registry ---------- 

static node_t *slot_for(const char *name)
{
    node_t *free_slot = NULL;
    for (int i = 0; i < MAX_NODES; i++) {
        if (g_nodes[i].used && strcmp(g_nodes[i].name, name) == 0)
            return &g_nodes[i];
        if (!g_nodes[i].used && !free_slot) free_slot = &g_nodes[i];
    }
    return free_slot;
}

static int cmp_nodes(const void *a, const void *b)
{
    return strcmp(((const node_t *)a)->name, ((const node_t *)b)->name);
}

// ---------- message handlers ---------- 

static void send_error(conn_t *c, const char *reason)
{
    kvbuf b;
    kv_init(&b);
    kv_add(&b, "reason", reason);
    if (!b.err) fs_send(c->fd, g_key, g_klen, MSG_ERROR, b.p, b.len);
    kv_free(&b);
}

static void handle_status(conn_t *c)
{
    node_t *snap = malloc(sizeof(node_t) * MAX_NODES);
    if (!snap) return;

    int n = 0;
    time_t now = time(NULL);

    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < MAX_NODES; i++)
        if (g_nodes[i].used) snap[n++] = g_nodes[i];
    pthread_mutex_unlock(&g_mu);

    qsort(snap, (size_t)n, sizeof *snap, cmp_nodes);

    kvbuf b;
    kv_init(&b);
    kv_addf(&b, "version", "%s", FS_VERSION_STR);
    kv_addf(&b, "uptime", "%ld", (long)(now - g_start));
    kv_addf(&b, "nodes", "%d", n);
    for (int i = 0; i < n; i++) {
        char k[32];
        node_t *x = &snap[i];
#define ADD(field, fmt, val) \
        do { snprintf(k, sizeof k, "n%d." field, i); kv_addf(&b, k, fmt, val); } while (0)
        ADD("name",   "%s",   x->name);
        ADD("state",  "%s",   x->online ? "online" : "offline");
        ADD("addr",   "%s",   x->addr);
        ADD("os",     "%s",   x->os);
        ADD("kernel", "%s",   x->kernel);
        ADD("distro", "%s",   x->distro);
        ADD("arch",   "%s",   x->arch);
        ADD("cores",  "%d",   x->cores);
        ADD("ram_mb", "%ld",  x->ram_mb);
        ADD("gpu",    "%s",   x->gpu);
        ADD("tools",  "%s",   x->tools);
        ADD("load",   "%.2f", x->load);
        ADD("age",    "%ld",  (long)(now - x->last_seen));
#undef ADD
    }
    free(snap);

    if (!b.err) fs_send(c->fd, g_key, g_klen, MSG_STATUS_RESP, b.p, b.len);
    kv_free(&b);
}

static void handle_agent(conn_t *c, const char *hello)
{
    char name[64] = "";
    kv_get(hello, "name", name, sizeof name);
    if (!cfg_name_ok(name)) {
        LOGW("%s: HELLO with invalid node name", c->peer);
        send_error(c, "invalid node name");
        return;
    }

    long interval = kv_get_long(hello, "interval", 5);
    if (interval < 1) interval = 1;
    if (interval > 3600) interval = 3600;
    int timeout = (int)(interval * 3);
    if (timeout < MIN_NODE_TIMEOUT) timeout = MIN_NODE_TIMEOUT;

    // ---- register ---- 
    pthread_mutex_lock(&g_mu);
    node_t *n = slot_for(name);
    if (!n) {
        pthread_mutex_unlock(&g_mu);
        LOGW("%s: node table full, rejecting '%s'", c->peer, name);
        send_error(c, "node table full");
        return;
    }
    int replaced = n->used && n->online;
    memset(n, 0, sizeof *n);
    n->used = 1;
    n->online = 1;
    n->gen = ++g_gen;
    fs_copy(n->name, sizeof n->name, name);
    kv_get(hello, "os", n->os, sizeof n->os);
    kv_get(hello, "kernel", n->kernel, sizeof n->kernel);
    kv_get(hello, "distro", n->distro, sizeof n->distro);
    kv_get(hello, "arch", n->arch, sizeof n->arch);
    kv_get(hello, "gpu", n->gpu, sizeof n->gpu);
    kv_get(hello, "tools", n->tools, sizeof n->tools);
    n->cores = (int)kv_get_long(hello, "cores", 0);
    n->ram_mb = kv_get_long(hello, "ram_mb", 0);
    n->load = kv_get_double(hello, "load", 0.0);
    fs_copy(n->addr, sizeof n->addr, c->peer);
    n->connected = n->last_seen = time(NULL);
    unsigned gen = n->gen;
    pthread_mutex_unlock(&g_mu);

    LOGI("node '%s' registered from %s (%d cores, %ld MB RAM)%s", name, c->peer,
         (int)kv_get_long(hello, "cores", 0), kv_get_long(hello, "ram_mb", 0),
         replaced ? " [replaced old connection]" : "");

    kvbuf b;
    kv_init(&b);
    kv_addf(&b, "coordinator", "fuscer-%s", FS_VERSION_STR);
    kv_add(&b, "node", name);
    kv_addf(&b, "heartbeat_timeout", "%d", timeout);
    if (b.err || fs_send(c->fd, g_key, g_klen, MSG_HELLO_OK, b.p, b.len) != FS_OK) {
        kv_free(&b);
        goto out;
    }
    kv_free(&b);

    // ---- heartbeat loop ---- 
    net_set_timeouts(c->fd, timeout);
    while (!fs_stop) {
        int type;
        char *body;
        size_t blen;
        int rc = fs_recv(c->fd, g_key, g_klen, &type, &body, &blen);
        if (rc != FS_OK) {
            LOGW("node '%s': %s", name, fs_strerror(rc));
            break;
        }
        if (type != MSG_HEARTBEAT) {
            LOGW("node '%s': unexpected message type %d", name, type);
            free(body);
            break;
        }

        int superseded = 0;
        pthread_mutex_lock(&g_mu);
        if (n->gen != gen) {
            superseded = 1;
        } else {
            n->load = kv_get_double(body, "load", n->load);
            n->last_seen = time(NULL);
        }
        pthread_mutex_unlock(&g_mu);
        free(body);

        if (superseded) {
            LOGI("node '%s': connection superseded by a newer one", name);
            break;
        }
        if (fs_send(c->fd, g_key, g_klen, MSG_HEARTBEAT_OK, "", 0) != FS_OK) break;
    }

out:
    pthread_mutex_lock(&g_mu);
    if (n->used && n->gen == gen) {
        n->online = 0;
        LOGI("node '%s' is offline", name);
    }
    pthread_mutex_unlock(&g_mu);
}

// ---------- connection thread ---------- 

static void *conn_thread(void *arg)
{
    conn_t *c = arg;

    net_set_timeouts(c->fd, 10);

    int type;
    char *body;
    size_t blen;
    int rc = fs_recv(c->fd, g_key, g_klen, &type, &body, &blen);

    if (rc == FS_ERR_AUTH) {
        LOGW("%s: authentication failed (wrong key?)", c->peer);
    } else if (rc != FS_OK) {
        LOGW("%s: first message: %s", c->peer, fs_strerror(rc));
    } else {
        switch (type) {
        case MSG_HELLO:      handle_agent(c, body);  break;
        case MSG_STATUS_REQ: handle_status(c);       break;
        default:
            LOGW("%s: unexpected first message type %d", c->peer, type);
            send_error(c, "unexpected message");
        }
        free(body);
    }

    close(c->fd);
    free(c);
    atomic_fetch_sub(&g_conns, 1);
    return NULL;
}

// ---------- main loop ---------- 

int coordinator_run(const fs_config *cfg)
{
    g_klen = strlen(cfg->key);
    memcpy(g_key, cfg->key, g_klen);
    g_start = time(NULL);

    int lfd = net_listen(cfg->listen);
    if (lfd < 0) return 1;
    LOGI("fuscer coordinator %s listening on %s", FS_VERSION_STR, cfg->listen);

    while (!fs_stop) {
        struct pollfd pfd = { .fd = lfd, .events = POLLIN };
        if (poll(&pfd, 1, 1000) <= 0) continue;

        struct sockaddr_storage ss;
        socklen_t sl = sizeof ss;
        int fd = accept(lfd, (struct sockaddr *)&ss, &sl);
        if (fd < 0) continue;

        if (atomic_load(&g_conns) >= MAX_CONNS) {
            close(fd);
            continue;
        }

        conn_t *c = calloc(1, sizeof *c);
        if (!c) { close(fd); continue; }
        c->fd = fd;
        net_addr_str((struct sockaddr *)&ss, sl, c->peer, sizeof c->peer);

        atomic_fetch_add(&g_conns, 1);

        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&at, 256 * 1024);
        pthread_t th;
        if (pthread_create(&th, &at, conn_thread, c) != 0) {
            LOGE("pthread_create failed");
            close(fd);
            free(c);
            atomic_fetch_sub(&g_conns, 1);
        }
        pthread_attr_destroy(&at);
    }

    LOGI("shutting down");
    close(lfd);
    return 0;
}
