#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "fuscer.h"
#include "net.h"
#include "proto.h"

// Exit statuses of `fuscer exec`, following timeout(1) / ssh conventions:
//   0..255   the remote command's own exit status
//   128+N    the remote command was killed by signal N
//   124      the command timed out
//   125      fuscer itself failed (bad node, exec disabled, network, ...)
//   130      interrupted locally (the remote command is killed)
#define EX_FUSCER   125
#define EX_TIMEOUT  124

static int write_all(int fd, const uint8_t *p, size_t n)
{
    while (n) {
        ssize_t r = write(fd, p, n);
        if (r < 0) {
            if (errno == EINTR && !fs_stop) continue;
            return -1;
        }
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

int exec_run(const fs_config *cfg, const char *node, const char *cmd, int timeout)
{
    const uint8_t *key = (const uint8_t *)cfg->key;
    size_t klen = strlen(cfg->key);
    size_t cl = strlen(cmd);

    if (cl == 0 || cl > FS_MAX_CMD) {
        fprintf(stderr, "fuscer: command is empty or longer than %d bytes\n", FS_MAX_CMD);
        return EX_FUSCER;
    }

    int fd = net_connect(cfg->coordinator, 5000);
    if (fd < 0) {
        fprintf(stderr, "fuscer: cannot connect to coordinator at %s\n", cfg->coordinator);
        return EX_FUSCER;
    }
    // A silent command is normal; the coordinator only speaks when there is output.
    net_set_timeouts(fd, timeout + 30);

    kvbuf b;
    kv_init(&b);
    kv_add(&b, "node", node);
    kv_addf(&b, "timeout", "%d", timeout);
    kv_add_hex(&b, "cmd", cmd, cl);
    int rc = b.err ? FS_ERR_IO : fs_send(fd, key, klen, MSG_EXEC_REQ, b.p, b.len);
    kv_free(&b);
    if (rc != FS_OK) {
        fprintf(stderr, "fuscer: %s\n", fs_strerror(rc));
        close(fd);
        return EX_FUSCER;
    }

    int status = EX_FUSCER;
    for (;;) {
        int type = 0;
        char *body = NULL;
        size_t blen = 0;
        rc = fs_recv(fd, key, klen, &type, &body, &blen);
        if (rc != FS_OK) {
            if (fs_stop) { status = 130; break; }
            if (rc == FS_ERR_CLOSED)
                fprintf(stderr, "fuscer: coordinator closed the connection (wrong key?)\n");
            else
                fprintf(stderr, "fuscer: %s\n", fs_strerror(rc));
            break;
        }

        if (type == MSG_EXEC_OUT) {
            char *hex = kv_dup(body, "data");
            if (hex) {
                size_t hl = strlen(hex);
                uint8_t *raw = malloc(hl / 2 + 1);
                long n = raw ? fs_hex_decode(hex, hl, raw, hl / 2) : -1;
                if (n > 0 && write_all(STDOUT_FILENO, raw, (size_t)n) != 0) {
                    free(raw); free(hex); free(body);
                    status = 141;             // stdout closed: stop, the socket close kills the job
                    break;
                }
                free(raw);
                free(hex);
            }
            free(body);
            continue;
        }

        if (type == MSG_EXEC_DONE) {
            long code = kv_get_long(body, "exit", -1);
            long sig = kv_get_long(body, "signal", 0);
            char reason[32] = "", error[160] = "";
            kv_get(body, "reason", reason, sizeof reason);
            kv_get(body, "error", error, sizeof error);
            free(body);

            if (error[0]) {
                fprintf(stderr, "fuscer: %s: %s\n", node, error);
                status = EX_FUSCER;
            } else if (!strcmp(reason, "timeout")) {
                fprintf(stderr, "fuscer: %s: timed out after %ds, command killed\n", node, timeout);
                status = EX_TIMEOUT;
            } else if (!strcmp(reason, "output limit")) {
                fprintf(stderr, "fuscer: %s: output limit exceeded, command killed\n", node);
                status = EX_FUSCER;
            } else if (sig > 0) {
                fprintf(stderr, "fuscer: %s: command killed by signal %ld\n", node, sig);
                status = 128 + (int)sig;
            } else {
                status = code >= 0 && code <= 255 ? (int)code : EX_FUSCER;
            }
            break;
        }

        if (type == MSG_ERROR) {
            char reason[128] = "unexpected reply";
            kv_get(body, "reason", reason, sizeof reason);
            fprintf(stderr, "fuscer: %s\n", reason);
        } else {
            fprintf(stderr, "fuscer: unexpected reply type %d\n", type);
        }
        free(body);
        break;
    }

    close(fd);
    return status;
}
