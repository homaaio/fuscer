#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "fuscer.h"
#include "net.h"
#include "proto.h"
#include "sysinfo.h"

// One connected session. Returns 1 if registration succeeded (so the caller
// can reset its backoff), 0 otherwise. Returns when the connection is lost.
static int session(int fd, const fs_config *cfg, const uint8_t *key, size_t klen)
{
    fs_caps caps;
    sys_collect_caps(&caps);

    kvbuf b;
    kv_init(&b);
    kv_add(&b, "name", cfg->name);
    kv_addf(&b, "interval", "%d", cfg->interval);
    kv_add(&b, "os", caps.os);
    kv_add(&b, "kernel", caps.kernel);
    kv_add(&b, "distro", caps.distro);
    kv_add(&b, "arch", caps.arch);
    kv_addf(&b, "cores", "%d", caps.cores);
    kv_addf(&b, "ram_mb", "%ld", caps.ram_mb);
    kv_add(&b, "gpu", caps.gpu);
    kv_add(&b, "tools", caps.tools);
    kv_addf(&b, "load", "%.2f", sys_load1());
    if (b.err) { kv_free(&b); return 0; }

    int rc = fs_send(fd, key, klen, MSG_HELLO, b.p, b.len);
    kv_free(&b);
    if (rc != FS_OK) {
        LOGW("send HELLO: %s", fs_strerror(rc));
        return 0;
    }

    int type;
    char *body;
    size_t blen;
    rc = fs_recv(fd, key, klen, &type, &body, &blen);
    if (rc == FS_ERR_CLOSED) {
        LOGW("coordinator closed the connection during handshake (wrong key?)");
        return 0;
    }
    if (rc != FS_OK) {
        LOGW("handshake: %s", fs_strerror(rc));
        return 0;
    }
    if (type == MSG_ERROR) {
        char reason[128] = "?";
        kv_get(body, "reason", reason, sizeof reason);
        LOGE("coordinator rejected registration: %s", reason);
        free(body);
        return 0;
    }
    if (type != MSG_HELLO_OK) {
        LOGW("unexpected reply type %d", type);
        free(body);
        return 0;
    }
    free(body);
    LOGI("registered as '%s' (%d cores, %ld MB RAM, tools: %s)",
         cfg->name, caps.cores, caps.ram_mb, caps.tools[0] ? caps.tools : "-");

    // heartbeat loop 
    while (!fs_stop) {
        fs_sleep(cfg->interval);
        if (fs_stop) break;

        char load[32];
        snprintf(load, sizeof load, "%.2f", sys_load1());
        kvbuf hb;
        kv_init(&hb);
        kv_add(&hb, "load", load);
        if (hb.err) { kv_free(&hb); break; }
        rc = fs_send(fd, key, klen, MSG_HEARTBEAT, hb.p, hb.len);
        kv_free(&hb);
        if (rc != FS_OK) {
            LOGW("send heartbeat: %s", fs_strerror(rc));
            break;
        }

        rc = fs_recv(fd, key, klen, &type, &body, &blen);
        if (rc != FS_OK) {
            if (!fs_stop) LOGW("heartbeat reply: %s", fs_strerror(rc));
            break;
        }
        free(body);
        if (type != MSG_HEARTBEAT_OK) {
            LOGW("unexpected reply type %d to heartbeat", type);
            break;
        }
    }
    return 1;
}

int agent_run(const fs_config *cfg)
{
    const uint8_t *key = (const uint8_t *)cfg->key;
    size_t klen = strlen(cfg->key);
    int backoff = 1;

    LOGI("fuscer agent %s, node '%s', coordinator %s",
         FS_VERSION_STR, cfg->name, cfg->coordinator);

    while (!fs_stop) {
        int fd = net_connect(cfg->coordinator, 5000);
        if (fd < 0) {
            LOGW("cannot connect to %s, retry in %ds", cfg->coordinator, backoff);
        } else {
            net_set_timeouts(fd, 10);
            if (session(fd, cfg, key, klen)) backoff = 1;
            close(fd);
            if (!fs_stop) LOGW("disconnected, reconnecting in %ds", backoff);
        }
        fs_sleep(backoff);
        if (backoff < 15) backoff *= 2;
    }

    LOGI("agent stopped");
    return 0;
}
