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

/*
 * 启动时调用：检查是否有未确认的升级。
 * 返回 1 表示已经回滚，调用方应退出，让外层守护重新拉起旧版本。
 */
int  ota_boot_check(void);

/* 运行健康后调用，确认升级成功，清掉 pending 标记 */
void ota_confirm(void);

#endif /* OTA_H */
