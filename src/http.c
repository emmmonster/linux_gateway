#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "http.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define HTTP_HDR_MAX 4096

static int parse_url(const char *url, char *host, size_t hcap,
                     char *port, size_t pcap, char *path, size_t pathcap)
{
    const char *p;
    const char *slash;
    const char *colon;

    if (strncmp(url, "http://", 7) != 0) {
        return -1;
    }
    p = url + 7;
    slash = strchr(p, '/');
    if (slash == NULL || slash == p) {
        return -1;
    }

    colon = (const char *)memchr(p, ':', (size_t)(slash - p));
    if (colon != NULL) {
        size_t hl = (size_t)(colon - p);
        size_t pl = (size_t)(slash - colon - 1);

        if (hl == 0 || hl >= hcap || pl == 0 || pl >= pcap) {
            return -1;
        }
        memcpy(host, p, hl);
        host[hl] = '\0';
        memcpy(port, colon + 1, pl);
        port[pl] = '\0';
    } else {
        size_t hl = (size_t)(slash - p);

        if (hl == 0 || hl >= hcap) {
            return -1;
        }
        memcpy(host, p, hl);
        host[hl] = '\0';
        snprintf(port, pcap, "80");
    }

    if (strlen(slash) >= pathcap) {
        return -1;
    }
    snprintf(path, pathcap, "%s", slash);
    return 0;
}

static int tcp_connect(const char *host, const char *port)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *rp;
    int fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port, &hints, &res) != 0) {
        fprintf(stderr, "[http] 解析 %s 失败\n", host);
        return -1;
    }

    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);
    return fd;
}

int http_download(const char *url, const char *out_path)
{
    char host[128];
    char port[8];
    char path[256];
    char req[512];
    char hdr[HTTP_HDR_MAX];
    char *sep;
    char *body;
    size_t hlen = 0;
    size_t body_len;
    int fd;
    int ret = -1;
    FILE *fp = NULL;
    ssize_t n;
    char status[16];

    if (url == NULL || out_path == NULL) {
        return -1;
    }
    if (parse_url(url, host, sizeof(host), port, sizeof(port), path, sizeof(path)) != 0) {
        fprintf(stderr, "[http] URL 不合法: %s\n", url);
        return -1;
    }

    fd = tcp_connect(host, port);
    if (fd < 0) {
        fprintf(stderr, "[http] 连接 %s:%s 失败\n", host, port);
        return -1;
    }

    /* HTTP/1.0 + Connection: close：服务端发完就断，不必处理分块传输 */
    n = snprintf(req, sizeof(req),
                 "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
                 path, host);
    if (n <= 0 || send(fd, req, (size_t)n, 0) != n) {
        fprintf(stderr, "[http] 发送请求失败\n");
        close(fd);
        return -1;
    }

    /* 先把响应头读完整 */
    while (hlen < sizeof(hdr) - 1) {
        n = recv(fd, hdr + hlen, sizeof(hdr) - 1 - hlen, 0);
        if (n <= 0) {
            break;
        }
        hlen += (size_t)n;
        hdr[hlen] = '\0';
        if ((sep = strstr(hdr, "\r\n\r\n")) != NULL) {
            break;
        }
    }
    hdr[hlen] = '\0';

    sep = strstr(hdr, "\r\n\r\n");
    if (sep == NULL) {
        fprintf(stderr, "[http] 响应头不完整\n");
        close(fd);
        return -1;
    }

    if (sscanf(hdr, "HTTP/%*s %15s", status) == 1 && strcmp(status, "200") != 0) {
        fprintf(stderr, "[http] 服务端返回 %s\n", status);
        close(fd);
        return -1;
    }

    body     = sep + 4;
    body_len = hlen - (size_t)(body - hdr);

    fp = fopen(out_path, "wb");
    if (fp == NULL) {
        fprintf(stderr, "[http] 无法创建 %s\n", out_path);
        close(fd);
        return -1;
    }

    if (body_len > 0 && fwrite(body, 1, body_len, fp) != body_len) {
        goto done;
    }

    for (;;) {
        static char buf[8192];
        n = recv(fd, buf, sizeof(buf), 0);
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "[http] 读取中断\n");
            goto done;
        }
        if (fwrite(buf, 1, (size_t)n, fp) != (size_t)n) {
            goto done;
        }
    }

    ret = 0;

done:
    fclose(fp);
    close(fd);

    if (ret != 0) {
        (void)unlink(out_path);
    }
    return ret;
}
