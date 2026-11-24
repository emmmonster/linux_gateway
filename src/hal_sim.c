#define _POSIX_C_SOURCE 200809L

#include "hal.h"
#include "frame.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SIM_PERIOD_MS 200

typedef struct {
    unsigned        seq;
    struct timespec next;
} sim_priv_t;

static void timespec_add_ms(struct timespec *ts, long ms)
{
    ts->tv_nsec += ms * 1000000L;
    while (ts->tv_nsec >= 1000000000L) {
        ts->tv_nsec -= 1000000000L;
        ts->tv_sec  += 1;
    }
}

static int sim_open(hal_t *h, const char *arg)
{
    sim_priv_t *p;

    (void)arg;

    p = (sim_priv_t *)calloc(1, sizeof(*p));
    if (p == NULL) {
        return -1;
    }
    clock_gettime(CLOCK_MONOTONIC, &p->next);
    h->priv = p;
    return 0;
}

static int sim_read(hal_t *h, uint8_t *buf, size_t len)
{
    sim_priv_t *p = (sim_priv_t *)h->priv;
    struct timespec now, nap;
    unsigned seq, i;
    int temp, hum, volt;
    uint16_t crc;

    if (p == NULL || len < FRAME_SIZE) {
        return -1;
    }

    /* 按固定周期出数据；真实设备靠串口阻塞，这里靠睡眠模拟 */
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > p->next.tv_sec ||
            (now.tv_sec == p->next.tv_sec && now.tv_nsec >= p->next.tv_nsec)) {
            break;
        }
        nap.tv_sec  = 0;
        nap.tv_nsec = 20 * 1000000L;
        nanosleep(&nap, NULL);
    }
    timespec_add_ms(&p->next, SIM_PERIOD_MS);

    seq  = p->seq++;
    temp = 200 + (int)(seq % 50);    /* 20.0 ~ 24.9 ℃ */
    hum  = 500 + (int)(seq % 100);   /* 50.0 ~ 59.9 % */
    volt = 3300 + (int)(seq % 20);   /* 3.300 ~ 3.319 V */

    buf[0] = 0xAA;
    buf[1] = 0x55;
    buf[2] = (uint8_t)(seq >> 8);
    buf[3] = (uint8_t)(seq & 0xFF);
    buf[4] = (uint8_t)(temp >> 8);
    buf[5] = (uint8_t)(temp & 0xFF);
    buf[6] = (uint8_t)(hum >> 8);
    buf[7] = (uint8_t)(hum & 0xFF);
    buf[8] = (uint8_t)(volt >> 8);
    buf[9] = (uint8_t)(volt & 0xFF);

    crc = 0;
    for (i = 0; i < 10; i++) {
        crc = (uint16_t)(crc + buf[i]);
    }
    buf[10] = (uint8_t)(crc >> 8);
    buf[11] = (uint8_t)(crc & 0xFF);

    return (int)FRAME_SIZE;
}

static int sim_write(hal_t *h, const uint8_t *buf, size_t len)
{
    size_t i;

    (void)h;
    if (buf == NULL) {
        return -1;
    }
    printf("[sim] 下发 %zu 字节:", len);
    for (i = 0; i < len; i++) {
        printf(" %02X", buf[i]);
    }
    printf("\n");
    return (int)len;
}

static void sim_close(hal_t *h)
{
    free(h->priv);
    h->priv = NULL;
    free(h);
}

hal_t *hal_sim_create(void)
{
    hal_t *h = (hal_t *)calloc(1, sizeof(*h));

    if (h == NULL) {
        return NULL;
    }
    h->open  = sim_open;
    h->read  = sim_read;
    h->write = sim_write;
    h->close = sim_close;
    return h;
}
