// Unit tests: SHA-256, HMAC-SHA256 (RFC 4231), framing, kv helpers. 
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "proto.h"
#include "sha256.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void hex(const uint8_t *d, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++) sprintf(out + i * 2, "%02x", d[i]);
}

static void test_sha(void)
{
    uint8_t d[32];
    char h[65];
    sha256_ctx c;

    sha256_init(&c);
    sha256_update(&c, "abc", 3);
    sha256_final(&c, d);
    hex(d, 32, h);
    CHECK(!strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));

    sha256_init(&c);
    sha256_final(&c, d);
    hex(d, 32, h);
    CHECK(!strcmp(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));

    // multi-block, fed in odd-sized chunks 
    const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256_init(&c);
    sha256_update(&c, m, 5);
    sha256_update(&c, m + 5, strlen(m) - 5);
    sha256_final(&c, d);
    hex(d, 32, h);
    CHECK(!strcmp(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
}

static void test_hmac(void)
{
    uint8_t d[32], key[131];
    char h[65];

    memset(key, 0x0b, 20);
    hmac_sha256(key, 20, "Hi There", 8, d);
    hex(d, 32, h);
    CHECK(!strcmp(h, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));

    hmac_sha256((const uint8_t *)"Jefe", 4, "what do ya want for nothing?", 28, d);
    hex(d, 32, h);
    CHECK(!strcmp(h, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));

    memset(key, 0xaa, 131); // key longer than the block size 
    const char *msg = "Test Using Larger Than Block-Size Key - Hash Key First";
    hmac_sha256(key, 131, msg, strlen(msg), d);
    hex(d, 32, h);
    CHECK(!strcmp(h, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"));
}

static void test_frames(void)
{
    const uint8_t key[] = "0123456789abcdef";
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    // round trip 
    kvbuf b;
    kv_init(&b);
    kv_add(&b, "name", "alpha");
    kv_add(&b, "evil", "x\ny=z");           // newline must not split the value 
    kv_addf(&b, "cores", "%d", 8);
    CHECK(!b.err);
    CHECK(fs_send(sv[0], key, 16, MSG_HELLO, b.p, b.len) == FS_OK);

    int type = 0;
    char *body = NULL;
    size_t blen = 0;
    CHECK(fs_recv(sv[1], key, 16, &type, &body, &blen) == FS_OK);
    CHECK(type == MSG_HELLO);
    CHECK(blen == b.len);

    char v[64];
    CHECK(kv_get(body, "name", v, sizeof v) && !strcmp(v, "alpha"));
    CHECK(kv_get(body, "evil", v, sizeof v) && !strcmp(v, "x y=z"));
    CHECK(kv_get_long(body, "cores", -1) == 8);
    CHECK(kv_get_long(body, "missing", 42) == 42);
    CHECK(!kv_get(body, "nam", v, sizeof v));   // prefix must not match 
    free(body);

    // empty body 
    CHECK(fs_send(sv[0], key, 16, MSG_STATUS_REQ, "", 0) == FS_OK);
    CHECK(fs_recv(sv[1], key, 16, &type, &body, &blen) == FS_OK);
    CHECK(type == MSG_STATUS_REQ && blen == 0 && body[0] == '\0');
    free(body);

    // wrong key 
    CHECK(fs_send(sv[0], key, 16, MSG_HELLO, b.p, b.len) == FS_OK);
    CHECK(fs_recv(sv[1], (const uint8_t *)"another-key-here", 16, &type, &body, &blen)
          == FS_ERR_AUTH);

    // tampered body: capture raw bytes, flip one bit, replay 
    CHECK(fs_send(sv[0], key, 16, MSG_HELLO, b.p, b.len) == FS_OK);
    uint8_t raw[1024];
    ssize_t n = read(sv[1], raw, sizeof raw);
    CHECK(n == (ssize_t)(FS_HDR_LEN + b.len + FS_MAC_LEN));
    raw[FS_HDR_LEN + 2] ^= 0x01;
    CHECK(write(sv[0], raw, (size_t)n) == n);
    CHECK(fs_recv(sv[1], key, 16, &type, &body, &blen) == FS_ERR_AUTH);

    // tampered type in header 
    CHECK(fs_send(sv[0], key, 16, MSG_HELLO, b.p, b.len) == FS_OK);
    n = read(sv[1], raw, sizeof raw);
    raw[5] = MSG_STATUS_REQ;
    CHECK(write(sv[0], raw, (size_t)n) == n);
    CHECK(fs_recv(sv[1], key, 16, &type, &body, &blen) == FS_ERR_AUTH);

    // garbage / bad magic 
    CHECK(write(sv[0], "GET / HTTP/1.1\r\n\r\n", 18) == 18);
    CHECK(fs_recv(sv[1], key, 16, &type, &body, &blen) == FS_ERR_PROTO);

    // oversized length field is rejected before allocating 
    uint8_t big[FS_HDR_LEN] = { 'F', 'S', 'C', 'R', FS_PROTO_VERSION, 1, 0, 0,
                                0xff, 0xff, 0xff, 0xff };
    CHECK(write(sv[0], big, sizeof big) == (ssize_t)sizeof big);
    CHECK(fs_recv(sv[1], key, 16, &type, &body, &blen) == FS_ERR_PROTO);

    // peer closes 
    close(sv[0]);
    CHECK(fs_recv(sv[1], key, 16, &type, &body, &blen) == FS_ERR_CLOSED);
    close(sv[1]);
    kv_free(&b);
}

static void test_hex(void)
{
    uint8_t out[8];
    CHECK(fs_hex_decode("00ff10Ab", 8, out, sizeof out) == 4);
    CHECK(out[0] == 0x00 && out[1] == 0xff && out[2] == 0x10 && out[3] == 0xab);
    CHECK(fs_hex_decode("abc", 3, out, sizeof out) == -1);          // odd length
    CHECK(fs_hex_decode("zz", 2, out, sizeof out) == -1);           // not hex
    CHECK(fs_hex_decode("", 0, out, sizeof out) == -1);             // empty
    CHECK(fs_hex_decode("0102030405", 10, out, 4) == -1);           // over the limit
    CHECK(fs_hex_decode("0102", 4, NULL, sizeof out) == 2);         // validate only

    // Bytes that a plain kv_add would mangle survive kv_add_hex, over a real frame.
    const uint8_t key[] = "0123456789abcdef";
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    const uint8_t raw[] = { 'a', '\n', 0, '\r', 0xff, '=', 'x', '\n', 'i', 'd', '=', '9' };
    kvbuf b;
    kv_init(&b);
    kv_addf(&b, "id", "%d", 5);
    kv_add_hex(&b, "data", raw, sizeof raw);
    CHECK(!b.err);
    CHECK(fs_send(sv[0], key, 16, MSG_EXEC_OUT, b.p, b.len) == FS_OK);

    int type = 0;
    char *body = NULL;
    size_t blen = 0;
    CHECK(fs_recv(sv[1], key, 16, &type, &body, &blen) == FS_OK);
    CHECK(type == MSG_EXEC_OUT);
    CHECK(kv_get_long(body, "id", -1) == 5);            // the "id=9" inside the data must not win
    char *hex = kv_dup(body, "data");
    CHECK(hex != NULL);
    uint8_t back[sizeof raw];
    CHECK(hex && fs_hex_decode(hex, strlen(hex), back, sizeof back) == (long)sizeof raw);
    CHECK(hex && memcmp(back, raw, sizeof raw) == 0);
    free(hex);
    free(body);
    kv_free(&b);
    close(sv[0]);
    close(sv[1]);

    // kv_dup has no length limit (kv_get truncates)
    size_t big = 50000;
    uint8_t *src = malloc(big), *dst = malloc(big);
    for (size_t i = 0; i < big; i++) src[i] = (uint8_t)(i * 7);
    kv_init(&b);
    kv_add_hex(&b, "data", src, big);
    hex = kv_dup(b.p, "data");
    CHECK(hex && strlen(hex) == big * 2);
    CHECK(hex && fs_hex_decode(hex, strlen(hex), dst, big) == (long)big);
    CHECK(hex && memcmp(src, dst, big) == 0);
    CHECK(kv_dup(b.p, "nope") == NULL);
    free(hex); free(src); free(dst);
    kv_free(&b);
}

int main(void)
{
    test_sha();
    test_hmac();
    test_frames();
    test_hex();
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    puts("test_proto: all ok");
    return 0;
}
