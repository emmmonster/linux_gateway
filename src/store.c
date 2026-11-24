#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "store.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define STORE_DIR_MAX 160
#define STORE_PATH_MAX 256

struct store {
    char     dir[STORE_DIR_MAX];
    uint32_t wseq;      /* 正在写的段号 */
    uint32_t woff;      /* 当前段写偏移 */
    int      wfd;
    uint32_t rseq;      /* 正在读的段号 */
    uint32_t roff;      /* 当前段读偏移 */
    int      rfd;
    size_t   last_len;  /* 上一次 peek 的记录长度，advance 用 */
};

static uint16_t crc16(const uint8_t *p, size_t n)
{
    uint16_t sum = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        sum = (uint16_t)(sum + p[i]);
    }
    return sum;
}

static int seg_path(const store_t *s, uint32_t seq, char *out, size_t cap)
{
    int n = snprintf(out, cap, "%s/seg_%06u.log", s->dir, (unsigned)seq);

    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

static int pos_path(const store_t *s, char *out, size_t cap)
{
    int n = snprintf(out, cap, "%s/pos", s->dir);

    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

static int write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t off = 0;

    while (off < len) {
        ssize_t n = write(fd, p + off, len - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

/* 扫描一段，返回合法数据的末尾偏移，并统计条数 */
static uint32_t scan_segment(int fd, uint32_t *count)
{
    uint8_t hdr[4];
    uint32_t off = 0;
    uint32_t cnt = 0;
    off_t size = lseek(fd, 0, SEEK_END);

    if (size < 0) {
        return 0;
    }

    while ((off_t)off + 4 <= size) {
        uint32_t len;

        if (pread(fd, hdr, 4, (off_t)off) != 4) {
            break;
        }
        len = ((uint32_t)hdr[0] << 8) | hdr[1];
        if (len == 0 || (off_t)(off + 4 + len) > size) {
            break;
        }
        off += 4 + len;
        cnt++;
    }

    if (off < (uint32_t)size) {          /* 截掉断电留下的半条 */
        if (ftruncate(fd, (off_t)off) != 0) {
            fprintf(stderr, "[store] 截断尾部失败\n");
        }
    }
    if (count != NULL) {
        *count = cnt;
    }
    return off;
}

static int open_wseg(store_t *s)
{
    char path[STORE_PATH_MAX];
    off_t size;

    if (seg_path(s, s->wseq, path, sizeof(path)) != 0) {
        return -1;
    }
    s->wfd = open(path, O_RDWR | O_CREAT | O_APPEND, 0644);
    if (s->wfd < 0) {
        return -1;
    }

    s->woff = (uint32_t)scan_segment(s->wfd, NULL);
    size = lseek(s->wfd, 0, SEEK_END);
    if (size != (off_t)s->woff) {
        return -1;
    }
    return 0;
}

static int open_rseg(store_t *s)
{
    char path[STORE_PATH_MAX];

    if (seg_path(s, s->rseq, path, sizeof(path)) != 0) {
        return -1;
    }
    s->rfd = open(path, O_RDONLY);
    if (s->rfd < 0) {
        return -1;
    }
    return 0;
}

static void save_pos(store_t *s)
{
    char path[STORE_PATH_MAX];
    char tmp[STORE_PATH_MAX + 8];
    char body[64];
    int n;
    int fd;

    if (pos_path(s, path, sizeof(path)) != 0) {
        return;
    }
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    n = snprintf(body, sizeof(body), "%u %u\n", (unsigned)s->rseq, (unsigned)s->roff);
    if (n <= 0) {
        return;
    }

    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return;
    }
    if (write_all(fd, body, (size_t)n) != 0) {
        close(fd);
        return;
    }
    fsync(fd);
    close(fd);
    (void)rename(tmp, path);
}

static void load_pos(store_t *s)
{
    char path[STORE_PATH_MAX];
    char body[64];
    int fd;
    ssize_t n;
    unsigned rs = 0;
    unsigned ro = 0;

    if (pos_path(s, path, sizeof(path)) != 0) {
        return;
    }
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return;
    }
    n = read(fd, body, sizeof(body) - 1);
    close(fd);
    if (n <= 0) {
        return;
    }
    body[n] = '\0';
    if (sscanf(body, "%u %u", &rs, &ro) == 2) {
        s->rseq = rs;
        s->roff = ro;
    }
}

/* 找出目录里最大段号 */
static int find_max_seg(const store_t *s, uint32_t *max_seq)
{
    DIR *d;
    struct dirent *e;
    uint32_t maxv = 0;
    int found = 0;

    d = opendir(s->dir);
    if (d == NULL) {
        return -1;
    }

    while ((e = readdir(d)) != NULL) {
        unsigned seq;

        if (sscanf(e->d_name, "seg_%u.log", &seq) != 1) {
            continue;
        }
        if (!found || seq > maxv) {
            maxv = seq;
            found = 1;
        }
    }
    closedir(d);

    if (!found) {
        return 1;      /* 目录是空的 */
    }
    *max_seq = maxv;
    return 0;
}

store_t *store_create(const char *dir)
{
    store_t *s;

    if (dir == NULL) {
        return NULL;
    }

    s = (store_t *)calloc(1, sizeof(*s));
    if (s == NULL) {
        return NULL;
    }
    s->wfd = -1;
    s->rfd = -1;

    snprintf(s->dir, sizeof(s->dir), "%s", dir);
    if (mkdir(s->dir, 0755) != 0 && errno != EEXIST) {
        free(s);
        return NULL;
    }

    load_pos(s);

    {
        uint32_t max_seq = 0;
        int r = find_max_seg(s, &max_seq);

        if (r == 1) {              /* 空目录，从第 0 段开始 */
            s->wseq = 0;
        } else if (r == 0) {
            s->wseq = max_seq;
        } else {
            free(s);
            return NULL;
        }
    }

    if (open_wseg(s) != 0) {
        free(s);
        return NULL;
    }

    /* 位点越界（文件被删或断电），退回到当前段开头 */
    if (s->rseq > s->wseq || (s->rseq == s->wseq && s->roff > s->woff)) {
        s->rseq = s->wseq;
        s->roff = 0;
    }

    return s;
}

static int rotate(store_t *s)
{
    if (s->wfd >= 0) {
        fsync(s->wfd);
        close(s->wfd);
        s->wfd = -1;
    }
    s->wseq++;
    s->woff = 0;
    return open_wseg(s);
}

int store_append(store_t *s, const void *data, size_t len)
{
    uint8_t hdr[4];
    uint16_t c;

    if (s == NULL || data == NULL || len == 0 || len > 0xFF00) {
        return -1;
    }

    if ((uint32_t)(4 + len) > STORE_SEG_MAX - s->woff) {
        if (rotate(s) != 0) {
            return -1;
        }
    }

    c = crc16((const uint8_t *)data, len);
    hdr[0] = (uint8_t)(len >> 8);
    hdr[1] = (uint8_t)(len & 0xFF);
    hdr[2] = (uint8_t)(c >> 8);
    hdr[3] = (uint8_t)(c & 0xFF);

    if (write_all(s->wfd, hdr, 4) != 0) {
        return -1;
    }
    if (write_all(s->wfd, data, len) != 0) {
        return -1;
    }

    s->woff += 4 + (uint32_t)len;

    /* 掉电保护：每条都刷盘，5 条/秒的量级开销可以接受 */
    (void)fsync(s->wfd);
    return 0;
}

int store_peek(store_t *s, uint8_t *buf, size_t cap, size_t *out_len)
{
    uint8_t hdr[4];

    if (s == NULL || buf == NULL) {
        return -1;
    }

    for (;;) {
        size_t len;
        uint16_t want;

        if (s->rfd < 0) {
            if (s->rseq > s->wseq) {
                return 0;
            }
            if (open_rseg(s) != 0) {
                return 0;
            }
        }

        if (pread(s->rfd, hdr, 4, (off_t)s->roff) != 4) {
            if (s->rseq >= s->wseq) {
                return 0;                 /* 追平了 */
            }
            close(s->rfd);
            s->rfd = -1;
            s->rseq++;
            s->roff = 0;
            continue;
        }

        len  = ((size_t)hdr[0] << 8) | hdr[1];
        want = (uint16_t)(((uint16_t)hdr[2] << 8) | hdr[3]);

        if (len == 0) {
            return 0;
        }
        if (len > cap) {
            fprintf(stderr, "[store] 记录 %zu 超过缓冲 %zu\n", len, cap);
            return -1;
        }
        if (pread(s->rfd, buf, len, (off_t)(s->roff + 4)) != (ssize_t)len) {
            fprintf(stderr, "[store] 记录不完整，视为结束\n");
            return 0;
        }
        if (crc16(buf, len) != want) {
            fprintf(stderr, "[store] 校验失败，视为结束\n");
            return 0;
        }

        s->last_len = len;
        if (out_len != NULL) {
            *out_len = len;
        }
        return 1;
    }
}

int store_advance(store_t *s)
{
    char path[STORE_PATH_MAX];

    if (s == NULL) {
        return -1;
    }

    s->roff += 4 + (uint32_t)s->last_len;

    if (s->rfd >= 0) {
        off_t size = lseek(s->rfd, 0, SEEK_END);

        if (size >= 0 && (off_t)s->roff >= size && s->rseq < s->wseq) {
            close(s->rfd);
            s->rfd = -1;
            if (seg_path(s, s->rseq, path, sizeof(path)) == 0) {
                (void)unlink(path);       /* 整段消费完就删，防止无限增长 */
            }
            s->rseq++;
            s->roff = 0;
        }
    }

    save_pos(s);
    return 0;
}

void store_destroy(store_t *s)
{
    if (s == NULL) {
        return;
    }
    if (s->wfd >= 0) {
        fsync(s->wfd);
        close(s->wfd);
    }
    if (s->rfd >= 0) {
        close(s->rfd);
    }
    free(s);
}
