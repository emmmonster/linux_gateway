#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "frame.h"
#include "hal.h"
#include "ringbuf.h"

#define RB_CAP (64u * 1024u)

static uint8_t   g_mem[RB_CAP];
static ringbuf_t g_rb;
static hal_t    *g_hal;
static volatile sig_atomic_t g_run = 1;

static void on_signal(int sig)
{
    (void)sig;
    g_run = 0;
}

static void sleep_ms(long ms)
{
    struct timespec ts;

    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* 采集线程：设备 -> 环形缓冲 */
static void *producer(void *arg)
{
    uint8_t buf[512];
    unsigned long long bytes = 0;

    (void)arg;

    while (g_run) {
        int n = g_hal->read(g_hal, buf, sizeof(buf));

        if (n < 0) {
            fprintf(stderr, "[producer] 读取失败，退出\n");
            break;
        }
        if (n == 0) {
            continue;
        }

        {
            size_t off = 0;
            while (off < (size_t)n && g_run) {
                size_t w = rb_write(&g_rb, buf + off, (size_t)n - off);
                off   += w;
                bytes += w;
                if (w == 0) {
                    sleep_ms(1);   /* 缓冲满，让出 CPU */
                }
            }
        }
    }

    fprintf(stderr, "[producer] 共采集 %llu 字节\n", bytes);
    return NULL;
}

/* 处理线程：环形缓冲 -> 解帧 -> 打印（阶段 2 会换成 MQTT 上报） */
static void *consumer(void *arg)
{
    uint8_t buf[256];
    frame_parser_t fp;
    frame_t f;
    unsigned long long frames = 0;

    (void)arg;

    frame_parser_init(&fp);

    while (g_run) {
        size_t n = rb_read(&g_rb, buf, sizeof(buf));

        if (n == 0) {
            sleep_ms(2);
            continue;
        }

        {
            size_t i;
            for (i = 0; i < n; i++) {
                if (frame_parser_feed(&fp, &buf[i], 1, &f)) {
                    printf("[rx] seq=%-5u temp=%5.1fC hum=%5.1f%% volt=%.2fV\n",
                           (unsigned)f.seq,
                           (double)f.temp / 10.0,
                           (double)f.hum / 10.0,
                           (double)f.volt / 1000.0);
                    frames++;
                }
            }
        }
    }

    fprintf(stderr, "[consumer] 共解析 %llu 帧\n", frames);
    return NULL;
}

int main(int argc, char **argv)
{
    const char *mode = (argc > 1) ? argv[1] : "sim";
    const char *dev  = (argc > 2) ? argv[2] : NULL;
    pthread_t tp, tc;

    if (strcmp(mode, "uart") == 0) {
        g_hal = hal_uart_create();
    } else {
        g_hal = hal_sim_create();
    }
    if (g_hal == NULL) {
        fprintf(stderr, "创建 hal 失败\n");
        return 1;
    }
    if (g_hal->open(g_hal, dev) != 0) {
        fprintf(stderr, "打开设备失败: %s\n", mode);
        g_hal->close(g_hal);
        return 1;
    }
    if (rb_init(&g_rb, g_mem, RB_CAP) != 0) {
        fprintf(stderr, "环形缓冲初始化失败\n");
        g_hal->close(g_hal);
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (pthread_create(&tp, NULL, producer, NULL) != 0) {
        fprintf(stderr, "创建采集线程失败\n");
        g_hal->close(g_hal);
        return 1;
    }
    if (pthread_create(&tc, NULL, consumer, NULL) != 0) {
        fprintf(stderr, "创建处理线程失败\n");
        g_run = 0;
        pthread_join(tp, NULL);
        g_hal->close(g_hal);
        return 1;
    }

    fprintf(stderr, "[main] 已启动 (mode=%s)，Ctrl-C 退出\n", mode);

    while (g_run) {
        sleep_ms(100);
    }

    pthread_join(tp, NULL);
    pthread_join(tc, NULL);
    g_hal->close(g_hal);
    return 0;
}
