#ifndef MQTT_H
#define MQTT_H

#include <stddef.h>
#include <stdint.h>

/*
 * MQTT 3.1.1 最小客户端（只用 TCP socket，不依赖任何第三方库）。
 *
 * 支持：
 *   CONNECT / CONNACK
 *   PUBLISH QoS0
 *   PINGREQ / PINGRESP 保活
 *   接收下行 PUBLISH（用于远程控制指令）
 *
 * 线程约定：所有接口必须在同一个线程里调用，内部不加锁。
 */

#define MQTT_MAX_PACKET 1024

typedef struct mqtt_client mqtt_client_t;

typedef void (*mqtt_msg_cb)(const char *topic, const uint8_t *payload, size_t len);

mqtt_client_t *mqtt_create(void);

/* 成功返回 0；失败返回 -1 */
int mqtt_connect(mqtt_client_t *c, const char *host, uint16_t port,
                 const char *client_id, uint16_t keepalive_s);

/* 发布 QoS0 消息，成功返回 0 */
int mqtt_publish(mqtt_client_t *c, const char *topic,
                 const void *payload, size_t len);

/*
 * 收一个报文并处理，最多等 timeout_ms 毫秒。
 * 返回 1 表示收到一条 PUBLISH，0 表示没有数据，-1 表示连接出错。
 */
int mqtt_poll(mqtt_client_t *c, int timeout_ms);

/* 空闲超过 keepalive 的一半就发 PINGREQ */
void mqtt_keepalive(mqtt_client_t *c);

void mqtt_set_callback(mqtt_client_t *c, mqtt_msg_cb cb);

void mqtt_disconnect(mqtt_client_t *c);
void mqtt_destroy(mqtt_client_t *c);

#endif /* MQTT_H */
