#ifndef HTTP_H
#define HTTP_H

/*
 * 极简 HTTP/1.0 GET 下载器，只依赖 socket，可交叉编译到板子。
 * 不支持 HTTPS、不支持 chunked，够 OTA 场景用。
 */

/* 成功返回 0 */
int http_download(const char *url, const char *out_path);

#endif /* HTTP_H */
