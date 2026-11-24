#ifndef HAL_H
#define HAL_H

#include <stddef.h>
#include <stdint.h>

/*
 * 硬件抽象层：整个项目里唯一和具体设备绑定的地方。
 *
 * x86 上开发时用 hal_sim_create()，跑在板子上时换成 hal_uart_create()，
 * 上层代码一行都不用改。
 */
typedef struct hal hal_t;

struct hal {
    /* 成功返回 0 */
    int  (*open)(hal_t *h, const char *arg);

    /* 返回读到的字节数；0 表示暂无数据；<0 表示错误 */
    int  (*read)(hal_t *h, uint8_t *buf, size_t len);

    /* 返回写入的字节数；<0 表示错误 */
    int  (*write)(hal_t *h, const uint8_t *buf, size_t len);

    void (*close)(hal_t *h);

    void *priv;
};

/* 模拟设备：每 200ms 产生一帧 12 字节假数据，用于在虚拟机上开发 */
hal_t *hal_sim_create(void);

/* 真实串口：默认 /dev/ttyS1，9600 8N1 */
hal_t *hal_uart_create(void);

#endif /* HAL_H */
