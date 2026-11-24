#ifndef RINGBUF_H
#define RINGBUF_H

#include <stddef.h>
#include <stdint.h>

/*
 * 单生产者 / 单消费者 无锁环形缓冲。
 *
 * 规则：
 *   - 只有生产者线程调用 rb_write，只有消费者线程调用 rb_read
 *   - 容量必须是 2 的幂（用掩码代替取模）
 *   - head/tail 是自由递增的计数器，不取模，靠掩码定位
 *   - 缓冲内存由调用方提供，模块内部不分配
 */
typedef struct ringbuf {
    uint8_t       *buf;
    size_t         cap;    /* 容量，2 的幂 */
    size_t         mask;   /* cap - 1 */
    volatile size_t head;  /* 生产者写 */
    volatile size_t tail;  /* 消费者写 */
} ringbuf_t;

/* 成功返回 0，参数非法返回 -1 */
int rb_init(ringbuf_t *rb, uint8_t *mem, size_t cap);

/* 返回实际写入字节数，可能小于 len（缓冲满） */
size_t rb_write(ringbuf_t *rb, const uint8_t *data, size_t len);

/* 返回实际读出字节数，可能小于 len（数据不足） */
size_t rb_read(ringbuf_t *rb, uint8_t *out, size_t len);

/* 当前已用字节数 */
size_t rb_used(ringbuf_t *rb);

#endif /* RINGBUF_H */
