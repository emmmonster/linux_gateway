#include "frame.h"

#include <string.h>

static uint16_t checksum16(const uint8_t *p, size_t n)
{
    uint16_t sum = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        sum = (uint16_t)(sum + p[i]);
    }
    return sum;
}

void frame_parser_init(frame_parser_t *fp)
{
    if (fp != NULL) {
        fp->len = 0;
    }
}

int frame_parser_feed(frame_parser_t *fp, const uint8_t *data, size_t len, frame_t *out)
{
    size_t i;

    if (fp == NULL || data == NULL || out == NULL) {
        return 0;
    }

    for (i = 0; i < len; i++) {
        uint8_t b = data[i];

        /* 找帧头第一个字节 */
        if (fp->len == 0) {
            if (b == 0xAA) {
                fp->buf[fp->len++] = b;
            }
            continue;
        }

        /* 找帧头第二个字节 */
        if (fp->len == 1) {
            if (b == 0x55) {
                fp->buf[fp->len++] = b;
            } else if (b != 0xAA) {
                fp->len = 0;
            }
            continue;
        }

        fp->buf[fp->len++] = b;

        if (fp->len == FRAME_SIZE) {
            uint16_t want = checksum16(fp->buf, FRAME_SIZE - 2);
            uint16_t got  = (uint16_t)(((uint16_t)fp->buf[10] << 8) | fp->buf[11]);

            if (want == got) {
                out->seq  = (uint16_t)(((uint16_t)fp->buf[2] << 8) | fp->buf[3]);
                out->temp = (int16_t)(((uint16_t)fp->buf[4] << 8) | fp->buf[5]);
                out->hum  = (uint16_t)(((uint16_t)fp->buf[6] << 8) | fp->buf[7]);
                out->volt = (uint16_t)(((uint16_t)fp->buf[8] << 8) | fp->buf[9]);
                fp->len = 0;
                return 1;
            }

            /* 校验失败：丢掉首字节，重新找帧头，避免把后面的好数据一起扔掉 */
            memmove(fp->buf, fp->buf + 1, FRAME_SIZE - 1);
            fp->len = FRAME_SIZE - 1;
        }
    }

    return 0;
}
