#ifndef OTA_H
#define OTA_H

/*
 * 第一阶段的 OTA：下载 + 校验 + 落盘。
 * 切换版本、重启生效、失败回滚放在下一步。
 */

/* 成功返回 0 */
int ota_run(const char *url, const char *expect_sha256);

/*
 * 把已校验的升级包安装到 app/<version>/gateway，
 * 并用原子 rename 把 app/current 指向新版本。
 * 成功返回 0（不重启，下次启动生效）。
 */
int ota_install(const char *pkg_path, const char *version);

#endif /* OTA_H */
