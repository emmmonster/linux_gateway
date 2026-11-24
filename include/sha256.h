#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_HEX_SIZE 65

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  data[64];
    size_t   datalen;
} sha256_ctx;

void sha256_init(sha256_ctx *ctx);
void sha256_update(sha256_ctx *ctx, const void *data, size_t len);
void sha256_final(sha256_ctx *ctx, uint8_t out[32]);

/* 计算一段内存的 SHA256，输出 64 位小写十六进制 + 结尾 '\0' */
void sha256_hex(const void *data, size_t len, char out[SHA256_HEX_SIZE]);

/* 计算一个文件的 SHA256，成功返回 0 */
int sha256_file(const char *path, char out[SHA256_HEX_SIZE]);

#endif /* SHA256_H */
