/* sha256.h - dependency-free SHA-256 for host-side build tools.
 *
 * The corpus identity (ABI-3.1, BP-6) must be reproducible on any CI host
 * without OpenSSL, Python hashing in a hot loop, or a platform crypto API.
 */
#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[8];
    uint64_t total;
    unsigned char buf[64];
    size_t used;
} sha256_ctx;

void sha256_init(sha256_ctx *s);
void sha256_update(sha256_ctx *s, const void *data, size_t n);
void sha256_final(sha256_ctx *s, unsigned char out[32]);
void sha256_hex(const unsigned char digest[32], char out[65]);
/* Returns 0 and writes lowercase hex, or -1 with errno set by fopen. */
int sha256_file(const char *path, char out[65]);

#endif /* SHA256_H */
