#include "ringbuf.h"

#include <string.h>

int rb_init(ringbuf_t *rb, uint8_t *mem, size_t cap)
{
    if (rb == NULL || mem == NULL || cap < 2) {
        return -1;
    }
    if ((cap & (cap - 1)) != 0) {   /* 必须是 2 的幂 */
        return -1;
    }

    rb->buf  = mem;
    rb->cap  = cap;
    rb->mask = cap - 1;
    rb->head = 0;
    rb->tail = 0;
    return 0;
}

size_t rb_write(ringbuf_t *rb, const uint8_t *data, size_t len)
{
    size_t head, tail, space, pos, first;

    if (data == NULL || len == 0) {
        return 0;
    }

    /* head 只有本线程写，relaxed 就够；tail 是本线程读别人的，需要 acquire */
    head  = __atomic_load_n(&rb->head, __ATOMIC_RELAXED);
    tail  = __atomic_load_n(&rb->tail, __ATOMIC_ACQUIRE);
    space = rb->cap - (head - tail);

    if (len > space) {
        len = space;
    }
    if (len == 0) {
        return 0;
    }

    pos   = head & rb->mask;
    first = rb->cap - pos;
    if (first > len) {
        first = len;
    }

    memcpy(rb->buf + pos, data, first);
    if (len > first) {
        memcpy(rb->buf, data + first, len - first);
    }

    __atomic_store_n(&rb->head, head + len, __ATOMIC_RELEASE);
    return len;
}

size_t rb_read(ringbuf_t *rb, uint8_t *out, size_t len)
{
    size_t head, tail, used, pos, first;

    if (out == NULL || len == 0) {
        return 0;
    }

    tail = __atomic_load_n(&rb->tail, __ATOMIC_RELAXED);
    head = __atomic_load_n(&rb->head, __ATOMIC_ACQUIRE);
    used = head - tail;

    if (len > used) {
        len = used;
    }
    if (len == 0) {
        return 0;
    }

    pos   = tail & rb->mask;
    first = rb->cap - pos;
    if (first > len) {
        first = len;
    }

    memcpy(out, rb->buf + pos, first);
    if (len > first) {
        memcpy(out + first, rb->buf, len - first);
    }

    __atomic_store_n(&rb->tail, tail + len, __ATOMIC_RELEASE);
    return len;
}

size_t rb_used(ringbuf_t *rb)
{
    size_t head = __atomic_load_n(&rb->head, __ATOMIC_ACQUIRE);
    size_t tail = __atomic_load_n(&rb->tail, __ATOMIC_ACQUIRE);
    return head - tail;
}
