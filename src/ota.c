#define _POSIX_C_SOURCE 200809L

#include "ota.h"
#include "http.h"
#include "sha256.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define OTA_TMP "upgrade.pkg"

int ota_run(const char *url, const char *expect_sha256)
{
    char got[SHA256_HEX_SIZE];

    if (url == NULL || expect_sha256 == NULL) {
        return -1;
    }

    printf("[ota] 下载 %s\n", url);
    if (http_download(url, OTA_TMP) != 0) {
        fprintf(stderr, "[ota] 下载失败\n");
        (void)unlink(OTA_TMP);
        return -1;
    }

    if (sha256_file(OTA_TMP, got) != 0) {
        fprintf(stderr, "[ota] 读取升级包失败\n");
        (void)unlink(OTA_TMP);
        return -1;
    }

    printf("[ota] 实际 %s\n", got);
    printf("[ota] 期望 %s\n", expect_sha256);

    if (strcmp(got, expect_sha256) != 0) {
        fprintf(stderr, "[ota] 校验不通过，丢弃\n");
        (void)unlink(OTA_TMP);
        return -1;
    }

    printf("[ota] 校验通过，已落盘 %s\n", OTA_TMP);
    return 0;
}

/* 版本名只允许字母数字和 . _ -，防止拼出 ../ 之类的路径 */
static int valid_version(const char *v)
{
    size_t i;

    if (v == NULL || v[0] == '\0' || strlen(v) > 32) {
        return 0;
    }
    for (i = 0; v[i] != '\0'; i++) {
        char c = v[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) {
            return 0;
        }
    }
    return 1;
}

static int copy_file(const char *src, const char *dst, mode_t mode)
{
    char    buf[8192];
    FILE   *in  = NULL;
    int     out = -1;
    size_t  n;
    int     ret = -1;

    in = fopen(src, "rb");
    if (in == NULL) {
        fprintf(stderr, "[ota] 打开 %s 失败\n", src);
        return -1;
    }
    out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (out < 0) {
        fprintf(stderr, "[ota] 创建 %s 失败\n", dst);
        fclose(in);
        return -1;
    }

    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        size_t off = 0;
        while (off < n) {
            ssize_t w = write(out, buf + off, n - off);
            if (w < 0) {
                if (errno == EINTR) {
                    continue;
                }
                goto done;
            }
            off += (size_t)w;
        }
    }
    if (ferror(in)) {
        goto done;
    }

    if (fchmod(out, mode) != 0) {
        goto done;
    }
    if (fsync(out) != 0) {
        goto done;
    }
    ret = 0;

done:
    close(out);
    fclose(in);
    if (ret != 0) {
        (void)unlink(dst);
    }
    return ret;
}

/* 从 /proc/self/exe 解析当前版本目录名：app/v2/gateway -> v2 */
static int ota_current_version(char *out, size_t cap)
{
    char path[320];
    char *slash;
    char *prev;
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);

    if (n <= 0) {
        return -1;
    }
    path[n] = '\0';

    slash = strrchr(path, '/');
    if (slash == NULL) {
        return -1;
    }
    *slash = '\0';

    prev = strrchr(path, '/');
    if (prev == NULL) {
        return -1;
    }
    prev++;

    if (strlen(prev) >= cap) {
        return -1;
    }
    snprintf(out, cap, "%s", prev);
    return 0;
}

/* 读 app/current 指向的版本名 */
static int read_current(char *out, size_t cap)
{
    ssize_t n;

    if (cap < 2) {
        return -1;
    }
    n = readlink("app/current", out, cap - 1);
    if (n <= 0) {
        return -1;
    }
    out[n] = '\0';
    return 0;
}

static int write_pending(const char *newv, const char *oldv)
{
    FILE *fp = fopen("app/pending", "w");

    if (fp == NULL) {
        return -1;
    }
    fprintf(fp, "%s %s\n", newv, oldv);
    fclose(fp);
    return 0;
}

int ota_install(const char *pkg_path, const char *version)
{
    char dir[160];
    char target[192];
    char link_tmp[160];
    char old_ver[64];

    if (pkg_path == NULL || !valid_version(version)) {
        fprintf(stderr, "[ota] 版本名不合法\n");
        return -1;
    }

    old_ver[0] = '\0';
    if (read_current(old_ver, sizeof(old_ver)) != 0) {
        snprintf(old_ver, sizeof(old_ver), "%s", version);
    }

    (void)mkdir("app", 0755);
    snprintf(dir, sizeof(dir), "app/%s", version);
    (void)mkdir(dir, 0755);

    snprintf(target, sizeof(target), "%s/gateway", dir);
    if (copy_file(pkg_path, target, 0755) != 0) {
        fprintf(stderr, "[ota] 写入新版本失败\n");
        return -1;
    }

    /* 先记下待验证的版本对，回滚时要用 */
    if (write_pending(version, old_ver) != 0) {
        fprintf(stderr, "[ota] 写升级标记失败\n");
        return -1;
    }

    /*
     * 原子切换：先建临时链接，再 rename 覆盖 current。
     * rename 在同一文件系统内是原子的，不存在“半个状态”。
     */
    snprintf(link_tmp, sizeof(link_tmp), "app/.current.%d", (int)getpid());
    (void)unlink(link_tmp);
    if (symlink(version, link_tmp) != 0) {
        fprintf(stderr, "[ota] 创建临时链接失败\n");
        return -1;
    }
    if (rename(link_tmp, "app/current") != 0) {
        fprintf(stderr, "[ota] 切换 current 失败\n");
        (void)unlink(link_tmp);
        return -1;
    }

    printf("[ota] 已安装到 %s\n", target);
    printf("[ota] app/current -> %s（旧版本 %s）\n", version, old_ver);
    printf("[ota] 重启后生效：./app/current/gateway sim localhost\n");
    return 0;
}

int ota_boot_check(void)
{
    char cur[64];
    char newv[64];
    char oldv[64];
    char tmp[96];
    FILE *fp;

    if (ota_current_version(cur, sizeof(cur)) != 0) {
        return 0;                       /* 不在 app/ 下运行，跳过 */
    }

    fp = fopen("app/pending", "r");
    if (fp == NULL) {
        return 0;                       /* 没有待验证的升级 */
    }
    if (fscanf(fp, "%63s %63s", newv, oldv) != 2) {
        fclose(fp);
        (void)unlink("app/pending");
        return 0;
    }
    fclose(fp);

    if (strcmp(cur, newv) != 0) {
        (void)unlink("app/pending");    /* 跑的不是新版本，标记作废 */
        return 0;
    }

    if (access("app/attempted", F_OK) == 0) {
        /* 第二次启动还没确认，说明上次没跑起来，回滚 */
        fprintf(stderr, "[ota] 新版本 %s 启动后未确认，回滚到 %s\n", newv, oldv);

        snprintf(tmp, sizeof(tmp), "app/.rollback.%d", (int)getpid());
        (void)unlink(tmp);
        if (symlink(oldv, tmp) == 0 && rename(tmp, "app/current") == 0) {
            (void)unlink("app/pending");
            (void)unlink("app/attempted");
            fprintf(stderr, "[ota] 已回滚，重启后将运行 %s\n", oldv);
            return 1;
        }
        (void)unlink(tmp);
        return 0;
    }

    fp = fopen("app/attempted", "w");
    if (fp != NULL) {
        fputs("1\n", fp);
        fclose(fp);
    }
    fprintf(stderr, "[ota] 首次启动 %s，等待确认\n", cur);
    return 0;
}

void ota_confirm(void)
{
    if (access("app/pending", F_OK) != 0) {
        return;
    }
    (void)unlink("app/pending");
    (void)unlink("app/attempted");
    fprintf(stderr, "[ota] 新版本运行正常，升级已确认\n");
}
