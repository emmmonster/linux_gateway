#ifndef STORE_H
#define STORE_H

#include <stddef.h>
#include <stdint.h>

/*
 * 断网缓存：分段追加日志。
 *
 *   queue/seg_000000.log
 *   queue/seg_000001.log
 *   ...
 *   queue/pos            读位点 "rseq roff"
 *
 * 每条记录格式：  [len:2][crc:2][data:len]   大端
 *
 * 设计要点：
 *   - 写入永远是追加，不做随机寻址，断电最多丢最后一条
 *   - 每段写满 1MB 就切新段，读完的整段直接 unlink，不会无限增长
 *   - 读位点单独存文件，消费成功后才推进，保证"发出去才删"
 *   - 打开时扫描当前段，把断电留下的半条记录截掉
 */

#define STORE_SEG_MAX (1024u * 1024u)

typedef struct store store_t;

/* 成功返回句柄，失败返回 NULL */
store_t *store_create(const char *dir);

/* 追加一条，成功返回 0 */
int  store_append(store_t *s, const void *data, size_t len);

/*
 * 读最老的一条但不推进位点。
 * 返回 1 读到，0 没有数据，-1 参数或数据错误。
 */
int  store_peek(store_t *s, uint8_t *buf, size_t cap, size_t *out_len);

/* 确认上一条已处理完，推进位点并持久化，成功返回 0 */
int  store_advance(store_t *s);

void store_destroy(store_t *s);

#endif /* STORE_H */
