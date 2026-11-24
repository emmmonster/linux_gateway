#ifndef FRAME_H
#define FRAME_H

#include <stddef.h>
#include <stdint.h>

/*
 * 帧格式（12 字节，大端）：
 *   0  1   : 帧头 AA 55
 *   2  3   : 序号 seq
 *   4  5   : 温度，单位 0.1℃（有符号）
 *   6  7   : 湿度，单位 0.1%
 *   8  9   : 电压，单位 mV
 *   10 11  : 校验，前 10 字节求和
 */
#define FRAME_SIZE 12

typedef struct {
    uint16_t seq;
    int16_t  temp;
    uint16_t hum;
    uint16_t volt;
} frame_t;

typedef struct {
    uint8_t buf[FRAME_SIZE];
    size_t  len;
} frame_parser_t;

void frame_parser_init(frame_parser_t *fp);

/*
 * 喂入任意长度的字节（可以一次喂 1 字节，也可以喂一大块）。
 * 返回 1 表示凑够一帧且校验通过，结果写入 out；返回 0 表示还没有完整帧。
 *
 * 内部处理三件事：粘包（多帧连在一起）、半包（帧被截断）、
 * 校验失败后的重同步。
 */
int frame_parser_feed(frame_parser_t *fp, const uint8_t *data, size_t len, frame_t *out);

#endif /* FRAME_H */
