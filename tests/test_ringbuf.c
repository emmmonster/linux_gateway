#include <stdio.h>
#include <string.h>

#include "ringbuf.h"

/* 不用 assert：Release 下 NDEBUG 会把它整个去掉，测试就变成空跑了 */
#define CHECK(cond)                                              \
    do {                                                         \
        if (!(cond)) {                                           \
            printf("FAIL: %s (line %d)\n", #cond, __LINE__);     \
            return 1;                                            \
        }                                                        \
    } while (0)

int main(void)
{
    static uint8_t mem[8];
    ringbuf_t rb;
    uint8_t out[16];

    /* 容量必须是 2 的幂 */
    CHECK(rb_init(&rb, mem, 7) != 0);
    CHECK(rb_init(&rb, mem, 8) == 0);

    /* 基本读写 */
    CHECK(rb_write(&rb, (const uint8_t *)"abcde", 5) == 5);
    CHECK(rb_used(&rb) == 5);

    /* 写满 */
    CHECK(rb_write(&rb, (const uint8_t *)"fgh", 3) == 3);
    CHECK(rb_used(&rb) == 8);
    CHECK(rb_write(&rb, (const uint8_t *)"x", 1) == 0);   /* 满了写不进 */

    /* 读一部分 */
    CHECK(rb_read(&rb, out, 6) == 6);
    CHECK(memcmp(out, "abcdef", 6) == 0);
    CHECK(rb_used(&rb) == 2);

    /* 回绕：这次写入会跨过缓冲尾部 */
    CHECK(rb_write(&rb, (const uint8_t *)"ijkl", 4) == 4);

    /* 剩下的 6 字节应该是 g h i j k l */
    CHECK(rb_read(&rb, out, 6) == 6);
    CHECK(memcmp(out, "ghijkl", 6) == 0);
    CHECK(rb_used(&rb) == 0);

    /* 空了读不到东西 */
    CHECK(rb_read(&rb, out, 4) == 0);

    printf("test_ringbuf: 全部通过\n");
    return 0;
}
