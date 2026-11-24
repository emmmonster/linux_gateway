#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "mqtt.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

struct mqtt_client {
    int         fd;
    uint8_t     rx[MQTT_MAX_PACKET];
    size_t      rx_len;
    uint16_t    keepalive;
    time_t      last_tx;
    mqtt_msg_cb cb;
};

/* ---------- 底层收发 ---------- */

static int write_all(int fd, const uint8_t *buf, size_t len)
{
    size_t off = 0;

    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

/* 返回 1 读满，0 超时，-1 出错/对端关闭 */
static int read_exact(int fd, uint8_t *buf, size_t len, int timeout_ms)
{
    size_t off = 0;

    while (off < len) {
        fd_set rfds;
        struct timeval tv;
        int r;
        ssize_t n;

        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        r = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (r == 0) {
            return 0;
        }

        n = recv(fd, buf + off, len - off, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        off += (size_t)n;
    }
    return 1;
}

/* MQTT 剩余长度的变长编码，返回写入字节数 */
static size_t encode_remaining(uint8_t *buf, size_t len)
{
    size_t i = 0;

    do {
        uint8_t b = (uint8_t)(len % 128);
        len /= 128;
        if (len > 0) {
            b |= 0x80;
        }
        buf[i++] = b;
    } while (len > 0);

    return i;
}

/* ---------- 连接 ---------- */

static int tcp_connect(const char *host, uint16_t port)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *p;
    char portstr[8];
    int fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);

    if (getaddrinfo(host, portstr, &hints, &res) != 0) {
        fprintf(stderr, "[mqtt] 解析主机失败: %s\n", host);
        return -1;
    }

    for (p = res; p != NULL; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);
    return fd;
}

mqtt_client_t *mqtt_create(void)
{
    mqtt_client_t *c = (mqtt_client_t *)calloc(1, sizeof(*c));

    if (c == NULL) {
        return NULL;
    }
    c->fd = -1;
    return c;
}

int mqtt_connect(mqtt_client_t *c, const char *host, uint16_t port,
                 const char *client_id, uint16_t keepalive_s)
{
    uint8_t pkt[256];
    uint8_t body[256];
    size_t n = 0;
    size_t m = 0;
    size_t idlen;
    uint8_t ack[4];
    int r;

    if (c == NULL || host == NULL || client_id == NULL) {
        return -1;
    }

    idlen = strlen(client_id);
    if (idlen > 100) {
        return -1;
    }

    c->fd = tcp_connect(host, port);
    if (c->fd < 0) {
        fprintf(stderr, "[mqtt] 连接 %s:%u 失败\n", host, (unsigned)port);
        return -1;
    }

    /* 可变头 */
    m = 0;
    body[m++] = 0x00;
    body[m++] = 0x04;
    memcpy(body + m, "MQTT", 4);
    m += 4;
    body[m++] = 0x04;                       /* 协议级别 3.1.1 */
    body[m++] = 0x02;                       /* clean session */
    body[m++] = (uint8_t)(keepalive_s >> 8);
    body[m++] = (uint8_t)(keepalive_s & 0xFF);

    /* 载荷：client id */
    body[m++] = (uint8_t)(idlen >> 8);
    body[m++] = (uint8_t)(idlen & 0xFF);
    memcpy(body + m, client_id, idlen);
    m += idlen;

    /* 固定头 */
    pkt[n++] = 0x10;
    n += encode_remaining(pkt + n, m);
    memcpy(pkt + n, body, m);
    n += m;

    if (write_all(c->fd, pkt, n) != 0) {
        fprintf(stderr, "[mqtt] 发送 CONNECT 失败\n");
        return -1;
    }

    r = read_exact(c->fd, ack, 4, 5000);
    if (r != 1 || ack[0] != 0x20) {
        fprintf(stderr, "[mqtt] 未收到 CONNACK\n");
        return -1;
    }
    if (ack[3] != 0) {
        fprintf(stderr, "[mqtt] CONNACK 返回码 %u\n", (unsigned)ack[3]);
        return -1;
    }

    c->keepalive = keepalive_s;
    c->last_tx   = time(NULL);
    return 0;
}

/* ---------- 发布 ---------- */

int mqtt_publish(mqtt_client_t *c, const char *topic, const void *payload, size_t len)
{
    uint8_t hdr[8];
    uint8_t tlen[2];
    size_t tlen_sz;
    size_t rl;
    size_t hl = 0;

    if (c == NULL || c->fd < 0 || topic == NULL) {
        return -1;
    }

    tlen_sz = strlen(topic);
    rl = 2 + tlen_sz + len;

    hdr[hl++] = 0x30;                     /* PUBLISH, QoS0 */
    hl += encode_remaining(hdr + hl, rl);

    tlen[0] = (uint8_t)(tlen_sz >> 8);
    tlen[1] = (uint8_t)(tlen_sz & 0xFF);

    if (write_all(c->fd, hdr, hl) != 0 ||
        write_all(c->fd, tlen, 2) != 0 ||
        write_all(c->fd, (const uint8_t *)topic, tlen_sz) != 0) {
        return -1;
    }
    if (len > 0 && write_all(c->fd, (const uint8_t *)payload, len) != 0) {
        return -1;
    }

    c->last_tx = time(NULL);
    return 0;
}

/* ---------- 收包 ---------- */

int mqtt_poll(mqtt_client_t *c, int timeout_ms)
{
    uint8_t hdr;
    uint8_t lenb;
    size_t rl = 0;
    size_t mult = 1;
    size_t i;
    int r;
    int type;

    if (c == NULL || c->fd < 0) {
        return -1;
    }

    r = read_exact(c->fd, &hdr, 1, timeout_ms);
    if (r <= 0) {
        return r;
    }

    for (i = 0; i < 4; i++) {
        r = read_exact(c->fd, &lenb, 1, timeout_ms);
        if (r <= 0) {
            return r;
        }
        rl += (size_t)(lenb & 0x7F) * mult;
        if ((lenb & 0x80) == 0) {
            break;
        }
        mult *= 128;
    }

    if (rl > sizeof(c->rx)) {
        fprintf(stderr, "[mqtt] 报文过大: %zu\n", rl);
        return -1;
    }
    if (rl > 0) {
        r = read_exact(c->fd, c->rx, rl, timeout_ms);
        if (r <= 0) {
            return r;
        }
    }

    type = hdr >> 4;

    if (type == 3) {                       /* PUBLISH */
        size_t tlen = ((size_t)c->rx[0] << 8) | c->rx[1];
        size_t off  = 2 + tlen;
        int qos = (hdr >> 1) & 0x03;

        if (qos > 0) {
            off += 2;                      /* 跳过 packet id */
        }
        if (off > rl) {
            return -1;
        }

        if (c->cb != NULL) {
            char topic[128];
            size_t copy = tlen < sizeof(topic) - 1 ? tlen : sizeof(topic) - 1;
            memcpy(topic, c->rx + 2, copy);
            topic[copy] = '\0';
            c->cb(topic, c->rx + off, rl - off);
        }
        return 1;
    }

    return 0;
}

/* ---------- 保活 ---------- */

void mqtt_keepalive(mqtt_client_t *c)
{
    static const uint8_t pingreq[2] = { 0xC0, 0x00 };
    time_t now;

    if (c == NULL || c->fd < 0 || c->keepalive == 0) {
        return;
    }

    now = time(NULL);
    if (now - c->last_tx >= (time_t)(c->keepalive / 2)) {
        if (write_all(c->fd, pingreq, sizeof(pingreq)) == 0) {
            c->last_tx = now;
        }
    }
}

void mqtt_set_callback(mqtt_client_t *c, mqtt_msg_cb cb)
{
    if (c != NULL) {
        c->cb = cb;
    }
}

void mqtt_disconnect(mqtt_client_t *c)
{
    static const uint8_t disc[2] = { 0xE0, 0x00 };

    if (c == NULL || c->fd < 0) {
        return;
    }
    (void)write_all(c->fd, disc, sizeof(disc));
    close(c->fd);
    c->fd = -1;
}

void mqtt_destroy(mqtt_client_t *c)
{
    if (c == NULL) {
        return;
    }
    mqtt_disconnect(c);
    free(c);
}
