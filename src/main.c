#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "frame.h"
#include "hal.h"
#include "mqtt.h"
#include "ota.h"
#include "ringbuf.h"
#include "store.h"

#define RB_CAP (64u * 1024u)
#define RECONNECT_SEC 3

static uint8_t   g_mem[RB_CAP];
static ringbuf_t g_rb;
static hal_t    *g_hal;
static mqtt_client_t *g_mqtt = NULL;
static store_t       *g_store = NULL;
static const char    *g_topic = "gw/data";
static const char    *g_broker = NULL;
static const char    *g_client_id = "imx6ull-gw";
static time_t         g_last_try = 0;
static int            g_offline = 0;
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

/* 打印自己是从哪个路径启动的，用来确认 OTA 切换是否生效 */
static void print_self_path(void)
{
    char buf[256];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);

    if (n > 0) {
        buf[n] = '\0';
        fprintf(stderr, "[main] 运行位置 %s\n", buf);
    }
}

/* 收到云端下发的指令（阶段 2 只是打印） */
static void on_remote_cmd(const char *topic, const uint8_t *payload, size_t len)
{
    printf("[cmd] %s -> %.*s\n", topic, (int)len, (const char *)payload);
}

/* 断线后每 3 秒重试一次 */
static void maybe_reconnect(void)
{
    time_t now;

    if (g_mqtt != NULL || g_broker == NULL) {
        return;
    }
    now = time(NULL);
    if (now - g_last_try < RECONNECT_SEC) {
        return;
    }
    g_last_try = now;

    g_mqtt = mqtt_create();
    if (g_mqtt != NULL && mqtt_connect(g_mqtt, g_broker, 1883, g_client_id, 60) == 0) {
        mqtt_set_callback(g_mqtt, on_remote_cmd);
        fprintf(stderr, "[mqtt] 已连接 %s:1883\n", g_broker);
        if (g_offline) {
            fprintf(stderr, "[mqtt] 恢复在线，开始补发积压数据\n");
            g_offline = 0;
        }
    } else {
        mqtt_destroy(g_mqtt);
        g_mqtt = NULL;
    }
}

/* 在线就直接发，离线或失败就落盘 */
static void publish_or_store(const char *payload, size_t len)
{
    if (g_mqtt != NULL) {
        if (mqtt_publish(g_mqtt, g_topic, payload, len) == 0) {
            return;
        }
        fprintf(stderr, "[mqtt] 发送失败，转入离线缓存\n");
        mqtt_destroy(g_mqtt);
        g_mqtt = NULL;
    }

    if (g_store != NULL && store_append(g_store, payload, len) != 0) {
        fprintf(stderr, "[store] 写入失败\n");
    } else if (!g_offline) {
        fprintf(stderr, "[store] 离线中，数据改为写入本地缓存\n");
        g_offline = 1;
    }
}

/* 每次最多补发 20 条，避免长时间占用处理线程 */
static void drain_store(void)
{
    uint8_t buf[256];
    int budget = 20;

    while (budget-- > 0 && g_mqtt != NULL && g_store != NULL) {
        size_t n = 0;
        int r = store_peek(g_store, buf, sizeof(buf), &n);

        if (r <= 0) {
            break;
        }
        if (mqtt_publish(g_mqtt, g_topic, buf, n) != 0) {
            mqtt_destroy(g_mqtt);
            g_mqtt = NULL;
            break;
        }
        store_advance(g_store);
    }
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
                    sleep_ms(1);
                }
            }
        }
    }

    fprintf(stderr, "[producer] 共采集 %llu 字节\n", bytes);
    return NULL;
}

/* 处理线程：环形缓冲 -> 解帧 -> 上报（离线则落盘） */
static void *consumer(void *arg)
{
    uint8_t buf[256];
    frame_parser_t fp;
    frame_t f;
    unsigned long long frames = 0;

    (void)arg;

    frame_parser_init(&fp);

    while (g_run) {
        size_t n;

        maybe_reconnect();

        if (g_mqtt != NULL) {
            int r = mqtt_poll(g_mqtt, 0);
            if (r < 0) {
                fprintf(stderr, "[mqtt] 连接已断开\n");
                mqtt_destroy(g_mqtt);
                g_mqtt = NULL;
            } else {
                mqtt_keepalive(g_mqtt);
                drain_store();      /* 先把积压补完，再发新数据 */
            }
        }

        n = rb_read(&g_rb, buf, sizeof(buf));
        if (n == 0) {
            sleep_ms(2);
            continue;
        }

        {
            size_t i;
            for (i = 0; i < n; i++) {
                char json[160];
                int  jn;

                if (!frame_parser_feed(&fp, &buf[i], 1, &f)) {
                    continue;
                }
                frames++;

                jn = snprintf(json, sizeof(json),
                              "{\"seq\":%u,\"temp\":%.1f,\"hum\":%.1f,\"volt\":%.2f}",
                              (unsigned)f.seq,
                              (double)f.temp / 10.0,
                              (double)f.hum / 10.0,
                              (double)f.volt / 1000.0);
                if (jn > 0) {
                    publish_or_store(json, (size_t)jn);
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
    const char *dev  = NULL;
    pthread_t tp, tc;

    g_broker = (argc > 2) ? argv[2] : NULL;

    /* OTA 单次任务模式：gateway ota <url> <sha256> */
    if (strcmp(mode, "ota") == 0) {
        if (argc < 4) {
            fprintf(stderr, "用法: %s ota <url> <sha256>\n", argv[0]);
            return 1;
        }
        return (ota_run(argv[2], argv[3]) == 0) ? 0 : 1;
    }

    /* 安装模式：gateway install <升级包> <版本号> */
    if (strcmp(mode, "install") == 0) {
        if (argc < 4) {
            fprintf(stderr, "用法: %s install <升级包> <版本号>\n", argv[0]);
            return 1;
        }
        return (ota_install(argv[2], argv[3]) == 0) ? 0 : 1;
    }

    print_self_path();

    if (argc > 3) {
        g_topic = argv[3];
    }
    if (strcmp(mode, "uart") == 0 && argc > 4) {
        dev = argv[4];
    }

    g_store = store_create("queue");
    if (g_store == NULL) {
        fprintf(stderr, "[main] 离线缓存目录创建失败，断网数据将丢失\n");
    } else {
        uint8_t tmp[256];
        size_t n = 0;
        if (store_peek(g_store, tmp, sizeof(tmp), &n) == 1) {
            fprintf(stderr, "[main] 发现上次未补发的数据，将优先发送\n");
        }
    }

    if (g_broker != NULL) {
        maybe_reconnect();
    }

    if (strcmp(mode, "uart") == 0) {
        g_hal = hal_uart_create();
    } else {
        g_hal = hal_sim_create();
    }
    if (g_hal == NULL) {
        fprintf(stderr, "创建 hal 失败\n");
        store_destroy(g_store);
        return 1;
    }
    if (g_hal->open(g_hal, dev) != 0) {
        fprintf(stderr, "打开设备失败: %s\n", mode);
        g_hal->close(g_hal);
        mqtt_destroy(g_mqtt);
        store_destroy(g_store);
        return 1;
    }
    if (rb_init(&g_rb, g_mem, RB_CAP) != 0) {
        fprintf(stderr, "环形缓冲初始化失败\n");
        g_hal->close(g_hal);
        mqtt_destroy(g_mqtt);
        store_destroy(g_store);
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (pthread_create(&tp, NULL, producer, NULL) != 0) {
        fprintf(stderr, "创建采集线程失败\n");
        g_hal->close(g_hal);
        mqtt_destroy(g_mqtt);
        store_destroy(g_store);
        return 1;
    }
    if (pthread_create(&tc, NULL, consumer, NULL) != 0) {
        fprintf(stderr, "创建处理线程失败\n");
        g_run = 0;
        pthread_join(tp, NULL);
        g_hal->close(g_hal);
        mqtt_destroy(g_mqtt);
        store_destroy(g_store);
        return 1;
    }

    fprintf(stderr, "[main] 已启动 (mode=%s)，Ctrl-C 退出\n", mode);

    while (g_run) {
        sleep_ms(100);
    }

    pthread_join(tp, NULL);
    pthread_join(tc, NULL);
    mqtt_destroy(g_mqtt);
    store_destroy(g_store);
    g_hal->close(g_hal);
    return 0;
}
