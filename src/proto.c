#define _GNU_SOURCE
#include "proto.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "common.h"
#include "sha256.h"

static int write_all(int fd, const uint8_t *p, size_t n)
{
    while (n) {
        ssize_t r = send(fd, p, n, MSG_NOSIGNAL);
        if (r < 0) {
            if (errno == EINTR && !fs_stop) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return FS_ERR_TIMEOUT;
            return FS_ERR_IO;
        }
        p += r;
        n -= (size_t)r;
    }
    return FS_OK;
}

static int read_all(int fd, uint8_t *p, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(fd, p + got, n - got, 0);
        if (r == 0) return FS_ERR_CLOSED;
        if (r < 0) {
            if (errno == EINTR && !fs_stop) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return FS_ERR_TIMEOUT;
            return FS_ERR_IO;
        }
        got += (size_t)r;
    }
    return FS_OK;
}

int fs_send(int fd, const uint8_t *key, size_t klen, int type,
            const char *body, size_t blen)
{
    if (blen > FS_MAX_BODY) return FS_ERR_PROTO;

    size_t total = FS_HDR_LEN + blen + FS_MAC_LEN;
    uint8_t *m = malloc(total);
    if (!m) return FS_ERR_IO;

    memcpy(m, "FSCR", 4);
    m[4] = FS_PROTO_VERSION;
    m[5] = (uint8_t)type;
    m[6] = m[7] = 0;
    m[8]  = (uint8_t)(blen >> 24);
    m[9]  = (uint8_t)(blen >> 16);
    m[10] = (uint8_t)(blen >> 8);
    m[11] = (uint8_t)(blen);
    if (blen) memcpy(m + FS_HDR_LEN, body, blen);
    hmac_sha256(key, klen, m, FS_HDR_LEN + blen, m + FS_HDR_LEN + blen);

    int rc = write_all(fd, m, total);
    free(m);
    return rc;
}

int fs_recv(int fd, const uint8_t *key, size_t klen, int *type,
            char **body, size_t *blen)
{
    uint8_t hdr[FS_HDR_LEN];
    int rc = read_all(fd, hdr, sizeof hdr);
    if (rc) return rc;

    if (memcmp(hdr, "FSCR", 4) != 0 || hdr[4] != FS_PROTO_VERSION)
        return FS_ERR_PROTO;

    size_t len = (size_t)hdr[8] << 24 | (size_t)hdr[9] << 16 |
                 (size_t)hdr[10] << 8 | (size_t)hdr[11];
    if (len > FS_MAX_BODY) return FS_ERR_PROTO;

    uint8_t *buf = malloc(len + FS_MAC_LEN + 1);
    if (!buf) return FS_ERR_IO;

    rc = read_all(fd, buf, len + FS_MAC_LEN);
    if (rc) {
        free(buf);
        return rc;
    }

    uint8_t mac[FS_MAC_LEN];
    hmac_sha256_2(key, klen, hdr, sizeof hdr, buf, len, mac);
    if (!ct_equal(mac, buf + len, FS_MAC_LEN)) {
        free(buf);
        return FS_ERR_AUTH;
    }
    if (memchr(buf, 0, len)) {
        free(buf);
        return FS_ERR_PROTO;
    }

    buf[len] = '\0';
    *type = hdr[5];
    *body = (char *)buf;
    *blen = len;
    return FS_OK;
}

const char *fs_strerror(int rc)
{
    switch (rc) {
    case FS_OK:          return "ok";
    case FS_ERR_IO:      return "i/o error";
    case FS_ERR_CLOSED:  return "connection closed";
    case FS_ERR_TIMEOUT: return "timeout";
    case FS_ERR_AUTH:    return "authentication failed";
    case FS_ERR_PROTO:   return "protocol error";
    default:             return "unknown error";
    }
}

// ---- kvbuf ---- 

void kv_init(kvbuf *b) { memset(b, 0, sizeof *b); }

void kv_free(kvbuf *b)
{
    free(b->p);
    kv_init(b);
}

static void kv_put(kvbuf *b, const char *s, size_t n)
{
    if (b->err) return;
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 256;
        while (nc < b->len + n + 1) nc *= 2;
        char *np = realloc(b->p, nc);
        if (!np) { b->err = 1; return; }
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

void kv_add(kvbuf *b, const char *key, const char *val)
{
    kv_put(b, key, strlen(key));
    kv_put(b, "=", 1);
    for (const char *c = val; *c; c++) {
        char ch = (*c == '\n' || *c == '\r') ? ' ' : *c;
        kv_put(b, &ch, 1);
    }
    kv_put(b, "\n", 1);
}

void kv_addf(kvbuf *b, const char *key, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    kv_add(b, key, tmp);
}

// Locates the value of `key`; returns a pointer into body and its length.
static const char *kv_find(const char *body, const char *key, size_t *vlen)
{
    size_t kl = strlen(key);
    const char *p = body;

    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t ll = nl ? (size_t)(nl - p) : strlen(p);
        if (ll > kl && p[kl] == '=' && memcmp(p, key, kl) == 0) {
            *vlen = ll - kl - 1;
            return p + kl + 1;
        }
        p = nl ? nl + 1 : p + ll;
    }
    return NULL;
}

int kv_get(const char *body, const char *key, char *out, size_t outsz)
{
    size_t vl;
    const char *v = kv_find(body, key, &vl);
    if (!v) return 0;
    if (outsz) {
        if (vl > outsz - 1) vl = outsz - 1;
        memcpy(out, v, vl);
        out[vl] = '\0';
    }
    return 1;
}

char *kv_dup(const char *body, const char *key)
{
    size_t vl;
    const char *v = kv_find(body, key, &vl);
    if (!v) return NULL;
    char *r = malloc(vl + 1);
    if (!r) return NULL;
    memcpy(r, v, vl);
    r[vl] = '\0';
    return r;
}

long kv_get_long(const char *body, const char *key, long def)
{
    char v[64];
    if (!kv_get(body, key, v, sizeof v)) return def;
    char *end;
    long x = strtol(v, &end, 10);
    return (end == v) ? def : x;
}

double kv_get_double(const char *body, const char *key, double def)
{
    char v[64];
    if (!kv_get(body, key, v, sizeof v)) return def;
    char *end;
    double x = strtod(v, &end);
    return (end == v) ? def : x;
}

// ---- hex ----

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

long fs_hex_decode(const char *s, size_t slen, uint8_t *out, size_t outmax)
{
    if (slen == 0 || (slen & 1)) return -1;
    size_t n = slen / 2;
    if (n > outmax) return -1;
    for (size_t i = 0; i < n; i++) {
        int h = hexval((unsigned char)s[2 * i]);
        int l = hexval((unsigned char)s[2 * i + 1]);
        if (h < 0 || l < 0) return -1;
        if (out) out[i] = (uint8_t)(h << 4 | l);
    }
    return (long)n;
}

void kv_add_hex(kvbuf *b, const char *key, const void *data, size_t n)
{
    static const char D[] = "0123456789abcdef";
    const uint8_t *p = data;
    char tmp[512];

    kv_put(b, key, strlen(key));
    kv_put(b, "=", 1);
    while (n) {
        size_t k = n > sizeof tmp / 2 ? sizeof tmp / 2 : n;
        for (size_t i = 0; i < k; i++) {
            tmp[2 * i]     = D[p[i] >> 4];
            tmp[2 * i + 1] = D[p[i] & 15];
        }
        kv_put(b, tmp, k * 2);
        p += k;
        n -= k;
    }
    kv_put(b, "\n", 1);
}
