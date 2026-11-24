/* CRTSCTS 属于 BSD 扩展，需要 _DEFAULT_SOURCE；
   定义了 _POSIX_C_SOURCE 会关掉默认的 _DEFAULT_SOURCE，所以要显式打开 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include "hal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

typedef struct {
    int fd;
} uart_priv_t;

static int uart_open(hal_t *h, const char *arg)
{
    const char *dev = (arg != NULL) ? arg : "/dev/ttyS1";
    struct termios tio;
    uart_priv_t *p;
    int fd;

    fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        perror("open uart");
        return -1;
    }

    if (tcgetattr(fd, &tio) < 0) {
        close(fd);
        return -1;
    }

    /* raw 模式：不做任何字符转换（自己写 cfmakeraw 以免依赖扩展宏） */
    tio.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    tio.c_oflag &= ~OPOST;
    tio.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tio.c_cflag &= ~(CSIZE | PARENB | CRTSCTS);
    tio.c_cflag |= (CS8 | CLOCAL | CREAD);
    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 0;

    cfsetispeed(&tio, B9600);
    cfsetospeed(&tio, B9600);

    if (tcsetattr(fd, TCSANOW, &tio) < 0) {
        close(fd);
        return -1;
    }
    tcflush(fd, TCIOFLUSH);

    p = (uart_priv_t *)calloc(1, sizeof(*p));
    if (p == NULL) {
        close(fd);
        return -1;
    }
    p->fd = fd;
    h->priv = p;
    return 0;
}

static int uart_read(hal_t *h, uint8_t *buf, size_t len)
{
    uart_priv_t *p = (uart_priv_t *)h->priv;
    fd_set rfds;
    struct timeval tv;
    int r;
    ssize_t n;

    if (p == NULL || p->fd < 0) {
        return -1;
    }

    FD_ZERO(&rfds);
    FD_SET(p->fd, &rfds);
    tv.tv_sec  = 0;
    tv.tv_usec = 100 * 1000;   /* 100ms 超时，返回 0 让上层继续跑 */

    r = select(p->fd + 1, &rfds, NULL, NULL, &tv);
    if (r < 0) {
        return (errno == EINTR) ? 0 : -1;
    }
    if (r == 0) {
        return 0;
    }

    n = read(p->fd, buf, len);
    if (n < 0) {
        return (errno == EAGAIN || errno == EINTR) ? 0 : -1;
    }
    return (int)n;
}

static int uart_write(hal_t *h, const uint8_t *buf, size_t len)
{
    uart_priv_t *p = (uart_priv_t *)h->priv;
    ssize_t n;

    if (p == NULL || p->fd < 0 || buf == NULL) {
        return -1;
    }

    n = write(p->fd, buf, len);
    if (n < 0) {
        return (errno == EAGAIN) ? 0 : -1;
    }
    return (int)n;
}

static void uart_close(hal_t *h)
{
    uart_priv_t *p = (uart_priv_t *)h->priv;

    if (p != NULL) {
        if (p->fd >= 0) {
            close(p->fd);
        }
        free(p);
    }
    h->priv = NULL;
    free(h);
}

hal_t *hal_uart_create(void)
{
    hal_t *h = (hal_t *)calloc(1, sizeof(*h));

    if (h == NULL) {
        return NULL;
    }
    h->open  = uart_open;
    h->read  = uart_read;
    h->write = uart_write;
    h->close = uart_close;
    return h;
}
