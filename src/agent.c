#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common.h"
#include "fuscer.h"
#include "net.h"
#include "proto.h"
#include "sysinfo.h"

#define EXEC_CHUNK         16384
#define EXEC_MAX_OUT       (4L * 1024 * 1024)  // output beyond this kills the command
#define EXEC_MAX_TIMEOUT   3600
#define EXEC_TERM_GRACE_MS 3000                // SIGTERM -> SIGKILL

// The one command this agent may be running right now.
typedef struct {
    int         active;
    long        id;
    pid_t       pid;
    int         fd;                 // read end of the output pipe, -1 after EOF
    long long   start_ms, deadline_ms, kill_at_ms;
    int         stage;              // 0 running, 1 SIGTERM sent, 2 SIGKILL sent
    const char *reason;             // "", "timeout", "killed", "output limit"
    long        total;              // bytes of output forwarded
} job_t;

typedef struct {
    int             fd;
    const uint8_t  *key;
    size_t          klen;
    const fs_config *cfg;
    job_t           job;
} actx;

// ---------- exec ----------

static void kill_group(pid_t pid, int sig)
{
    if (kill(-pid, sig) < 0) kill(pid, sig);
}

static void begin_kill(job_t *j, const char *reason)
{
    if (j->stage != 0) return;
    j->reason = reason;
    j->stage = 1;
    j->kill_at_ms = fs_now_ms() + EXEC_TERM_GRACE_MS;
    kill_group(j->pid, SIGTERM);
}

static int send_done(actx *a, long id, int code, int sig, const char *reason,
                     const char *error, long ms)
{
    kvbuf b;
    kv_init(&b);
    kv_addf(&b, "id", "%ld", id);
    kv_addf(&b, "exit", "%d", code);
    kv_addf(&b, "signal", "%d", sig);
    kv_add(&b, "reason", reason ? reason : "");
    kv_add(&b, "error", error ? error : "");
    kv_addf(&b, "ms", "%ld", ms);
    int rc = b.err ? FS_ERR_IO : fs_send(a->fd, a->key, a->klen, MSG_EXEC_DONE, b.p, b.len);
    kv_free(&b);
    return rc;
}

static int send_out(actx *a, const uint8_t *data, size_t n)
{
    kvbuf b;
    kv_init(&b);
    kv_addf(&b, "id", "%ld", a->job.id);
    kv_add_hex(&b, "data", data, n);
    int rc = b.err ? FS_ERR_IO : fs_send(a->fd, a->key, a->klen, MSG_EXEC_OUT, b.p, b.len);
    kv_free(&b);
    return rc;
}

// Forward whatever is readable on the pipe. Returns 0, or -1 if the socket failed.
static int job_pump(actx *a, int max_chunks)
{
    job_t *j = &a->job;
    uint8_t buf[EXEC_CHUNK];

    while (j->fd >= 0 && max_chunks-- > 0) {
        ssize_t r = read(j->fd, buf, sizeof buf);
        if (r > 0) {
            size_t n = (size_t)r;
            if (j->total >= EXEC_MAX_OUT) continue;          // over the limit: discard
            if (j->total + (long)n > EXEC_MAX_OUT) n = (size_t)(EXEC_MAX_OUT - j->total);
            j->total += (long)n;
            if (send_out(a, buf, n) != FS_OK) return -1;
            if (j->total >= EXEC_MAX_OUT) begin_kill(j, "output limit");
        } else if (r == 0) {
            close(j->fd);
            j->fd = -1;
        } else if (errno == EINTR) {
            continue;
        } else {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                close(j->fd);
                j->fd = -1;
            }
            break;
        }
    }
    return 0;
}

// The command's shell has exited (zombie, not reaped yet): clean up and report.
static int job_finish(actx *a)
{
    job_t *j = &a->job;

    // Stragglers left behind (e.g. `sleep 100 &`) must not outlive the job.
    // The zombie still owns the process group id, so it cannot be recycled.
    kill_group(j->pid, SIGKILL);
    int prc = job_pump(a, 256);
    if (j->fd >= 0) { close(j->fd); j->fd = -1; }

    int st = 0;
    while (waitpid(j->pid, &st, 0) < 0 && errno == EINTR) {}

    int code = -1, sig = 0;
    if (WIFEXITED(st)) code = WEXITSTATUS(st);
    else if (WIFSIGNALED(st)) sig = WTERMSIG(st);
    long ms = (long)(fs_now_ms() - j->start_ms);

    LOGI("exec #%ld finished: exit=%d signal=%d%s%s (%ld ms, %ld bytes)", j->id, code, sig,
         j->reason && *j->reason ? " reason=" : "", j->reason ? j->reason : "", ms, j->total);

    j->active = 0;
    if (prc != 0) return -1;
    return send_done(a, j->id, code, sig, j->reason, "", ms) == FS_OK ? 0 : -1;
}

static int job_tick(actx *a)
{
    job_t *j = &a->job;
    siginfo_t si;
    memset(&si, 0, sizeof si);

    if (waitid(P_PID, (id_t)j->pid, &si, WEXITED | WNOHANG | WNOWAIT) == 0 &&
        si.si_pid == j->pid)
        return job_finish(a);

    long long now = fs_now_ms();
    if (j->stage == 0 && now >= j->deadline_ms) {
        begin_kill(j, "timeout");
    } else if (j->stage == 1 && now >= j->kill_at_ms) {
        kill_group(j->pid, SIGKILL);
        j->stage = 2;
    }
    return 0;
}

static void job_abort(actx *a)      // session ended while a command was running
{
    job_t *j = &a->job;
    if (!j->active) return;
    kill_group(j->pid, SIGKILL);
    if (j->fd >= 0) { close(j->fd); j->fd = -1; }
    while (waitpid(j->pid, NULL, 0) < 0 && errno == EINTR) {}
    LOGW("exec #%ld aborted: connection to the coordinator was lost", j->id);
    j->active = 0;
}

static void log_safe(char *dst, size_t n, const char *src)
{
    size_t i = 0;
    for (; src[i] && i + 1 < n; i++) {
        unsigned char c = (unsigned char)src[i];
        dst[i] = (c < 32 || c == 127) ? '?' : (char)c;
    }
    dst[i] = '\0';
}

// Handles MSG_EXEC_RUN. Returns 0, or -1 if the socket failed.
static int job_start(actx *a, const char *body)
{
    long id = kv_get_long(body, "id", -1);
    long to = kv_get_long(body, "timeout", 60);
    if (id < 0) return -1;
    if (to < 1) to = 1;
    if (to > EXEC_MAX_TIMEOUT) to = EXEC_MAX_TIMEOUT;

#define REFUSE(msg) return send_done(a, id, -1, 0, "", (msg), 0) == FS_OK ? 0 : -1

    if (!a->cfg->allow_exec) {
        LOGW("exec #%ld refused: exec is disabled in the agent config", id);
        REFUSE("exec is disabled on this node (agent config: exec = yes)");
    }
    if (a->job.active) REFUSE("node is busy");

    char *hex = kv_dup(body, "cmd");
    size_t hl = hex ? strlen(hex) : 0;
    uint8_t *cmd = malloc(hl / 2 + 1);
    long n = (hex && cmd) ? fs_hex_decode(hex, hl, cmd, FS_MAX_CMD) : -1;
    free(hex);
    if (n <= 0) { free(cmd); REFUSE("bad command"); }
    cmd[n] = '\0';
    if (strlen((char *)cmd) != (size_t)n) { free(cmd); REFUSE("bad command"); }

    int pfd[2];
    if (pipe2(pfd, O_CLOEXEC) < 0) { free(cmd); REFUSE("pipe() failed"); }

    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]); close(pfd[1]);
        free(cmd);
        REFUSE("fork() failed");
    }
    if (pid == 0) {
        setsid();                                  // own process group, no tty
        int nul = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (nul >= 0) dup2(nul, 0);
        dup2(pfd[1], 1);                           // stdout and stderr share the pipe
        dup2(pfd[1], 2);
        signal(SIGPIPE, SIG_DFL);                  // agent ignores it; commands must not
        unsetenv("FUSCER_KEY");                    // do not leak the cluster key
        execl("/bin/sh", "sh", "-c", (char *)cmd, (char *)NULL);
        _exit(127);
    }

    close(pfd[1]);
    fcntl(pfd[0], F_SETFL, O_NONBLOCK);

    job_t *j = &a->job;
    memset(j, 0, sizeof *j);
    j->active = 1;
    j->id = id;
    j->pid = pid;
    j->fd = pfd[0];
    j->start_ms = fs_now_ms();
    j->deadline_ms = j->start_ms + to * 1000LL;
    j->reason = "";

    char shown[121];
    log_safe(shown, sizeof shown, (char *)cmd);
    LOGI("exec #%ld started (pid %d, timeout %lds): %s", id, (int)pid, to, shown);
    free(cmd);
    return 0;
#undef REFUSE
}

// ---------- session ----------

// One connected session. Returns 1 if registration succeeded (so the caller
// can reset its backoff), 0 otherwise. Returns when the connection is lost.
static int session(int fd, const fs_config *cfg, const uint8_t *key, size_t klen)
{
    fcntl(fd, F_SETFD, FD_CLOEXEC);      // commands we spawn must not inherit the socket

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
    kv_add(&b, "exec", cfg->allow_exec ? "1" : "0");
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
    LOGI("registered as '%s' (%d cores, %ld MB RAM, tools: %s, exec: %s)",
         cfg->name, caps.cores, caps.ram_mb, caps.tools[0] ? caps.tools : "-",
         cfg->allow_exec ? "ON" : "off");

    actx a;
    memset(&a, 0, sizeof a);
    a.fd = fd;
    a.key = key;
    a.klen = klen;
    a.cfg = cfg;
    a.job.fd = -1;

    long long next_hb = fs_now_ms() + cfg->interval * 1000LL;
    long long await_since = 0;              // when the oldest unanswered heartbeat was sent
    int alive = 1;

    while (!fs_stop && alive) {
        long long now = fs_now_ms();
        long long wake = next_hb;
        if (a.job.active && now + 100 < wake) wake = now + 100;   // watch the child
        int to = wake > now ? (int)(wake - now) : 0;

        struct pollfd p[2];
        int np = 0;
        p[np].fd = fd; p[np].events = POLLIN; p[np].revents = 0; np++;
        if (a.job.active && a.job.fd >= 0) {
            p[np].fd = a.job.fd; p[np].events = POLLIN; p[np].revents = 0; np++;
        }

        int pr = poll(p, (nfds_t)np, to);
        if (pr < 0) {
            if (errno == EINTR) continue;
            LOGW("poll: %s", strerror(errno));
            break;
        }

        if (p[0].revents) {
            rc = fs_recv(fd, key, klen, &type, &body, &blen);
            if (rc != FS_OK) {
                if (!fs_stop) LOGW("coordinator: %s", fs_strerror(rc));
                break;
            }
            switch (type) {
            case MSG_HEARTBEAT_OK:
                await_since = 0;
                break;
            case MSG_EXEC_RUN:
                if (job_start(&a, body) != 0) alive = 0;
                break;
            case MSG_EXEC_KILL:
                if (a.job.active && kv_get_long(body, "id", -1) == a.job.id) {
                    LOGI("exec #%ld: kill requested", a.job.id);
                    begin_kill(&a.job, "killed");
                }
                break;
            case MSG_ERROR: {
                char reason[128] = "?";
                kv_get(body, "reason", reason, sizeof reason);
                LOGW("coordinator error: %s", reason);
                alive = 0;
                break;
            }
            default:
                LOGW("unexpected message type %d", type);
                alive = 0;
            }
            free(body);
            if (!alive) break;
        }

        if (a.job.active) {
            if (job_pump(&a, 16) != 0) break;
            if (a.job.active && job_tick(&a) != 0) break;
        }

        now = fs_now_ms();
        if (await_since && now - await_since > 10000) {
            LOGW("no heartbeat reply from the coordinator");
            break;
        }
        if (now >= next_hb) {
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
            if (!await_since) await_since = now;
            next_hb = now + cfg->interval * 1000LL;
        }
    }

    job_abort(&a);
    return 1;
}

int agent_run(const fs_config *cfg)
{
    const uint8_t *key = (const uint8_t *)cfg->key;
    size_t klen = strlen(cfg->key);
    int backoff = 1;

    LOGI("fuscer agent %s, node '%s', coordinator %s",
         FS_VERSION_STR, cfg->name, cfg->coordinator);
    if (cfg->allow_exec) {
        LOGW("remote exec is ENABLED: anyone who holds the shared key can run commands "
             "on this machine as uid %d", (int)geteuid());
        if (geteuid() == 0)
            LOGW("the agent is running as root - consider a dedicated unprivileged user");
    }

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
