#ifndef FS_PROTO_H
#define FS_PROTO_H

#include <stddef.h>
#include <stdint.h>

// Frame:  | magic "FSCR" | ver u8 | type u8 | flags u16 | len u32 |  body  | HMAC-SHA256 (32) |
//         '------------------ header, 12 bytes ------------------'
// All integers are big-endian. HMAC covers header + body.
// Body: text lines "key=value\n".

#define FS_PROTO_VERSION 1
#define FS_HDR_LEN       12
#define FS_MAC_LEN       32
#define FS_MAX_BODY      (256 * 1024)

enum {
    MSG_HELLO = 1,        // agent -> coordinator: register + capabilities 
    MSG_HELLO_OK,         // coordinator -> agent 
    MSG_HEARTBEAT,        // agent -> coordinator: load 
    MSG_HEARTBEAT_OK,     // coordinator -> agent 
    MSG_STATUS_REQ,       // client -> coordinator 
    MSG_STATUS_RESP,      // coordinator -> client 
    MSG_ERROR,            // either way: body has "reason=" 
    MSG_EXEC_REQ,         // client -> coordinator: node, timeout, cmd (hex) 
    MSG_EXEC_RUN,         // coordinator -> agent: id, timeout, cmd (hex) 
    MSG_EXEC_OUT,         // agent -> coordinator -> client: id, data (hex) 
    MSG_EXEC_DONE,        // agent -> coordinator -> client: id, exit, signal, reason, error, ms 
    MSG_EXEC_KILL         // coordinator -> agent: id 
};

#define FS_MAX_CMD  60000   // longest remote command line, bytes


enum {
    FS_OK = 0,
    FS_ERR_IO = -1,
    FS_ERR_CLOSED = -2,
    FS_ERR_TIMEOUT = -3,
    FS_ERR_AUTH = -4,
    FS_ERR_PROTO = -5
};

int fs_send(int fd, const uint8_t *key, size_t klen, int type,
            const char *body, size_t blen);

// On FS_OK, *body is a malloc'd NUL-terminated string (caller frees). 
int fs_recv(int fd, const uint8_t *key, size_t klen, int *type,
            char **body, size_t *blen);

const char *fs_strerror(int rc);

// ---- key=value body helpers ---- 
typedef struct {
    char  *p;
    size_t len, cap;
    int    err;
} kvbuf;

void kv_init(kvbuf *b);
void kv_free(kvbuf *b);
void kv_add(kvbuf *b, const char *key, const char *val);
void kv_addf(kvbuf *b, const char *key, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

// Returns 1 if found (value copied, truncated to outsz-1), else 0. 
int    kv_get(const char *body, const char *key, char *out, size_t outsz);
long   kv_get_long(const char *body, const char *key, long def);
double kv_get_double(const char *body, const char *key, double def);

// ---- hex (binary-safe values inside key=value bodies) ----
// Decodes into out (may be NULL to only validate). Returns the byte count,
// or -1 if the input is empty, has odd length, is not hex, or exceeds outmax.
long fs_hex_decode(const char *s, size_t slen, uint8_t *out, size_t outmax);

// Appends "key=<hex of data>\n" (no length limit, unlike kv_addf).
void kv_add_hex(kvbuf *b, const char *key, const void *data, size_t n);

// Like kv_get but returns a malloc'd copy of any length (NULL if absent).
char *kv_dup(const char *body, const char *key);

#endif
