/*
 * ota_update.h - 咖啡台 OTA 固件升级（网页上传）
 *
 * 通过 HTTP POST 把新固件二进制写入另一 app 分区（ota_0/ota_1），
 * 成功后切换启动分区并重启；升级过程失败自动回滚到原分区。
 */
#pragma once
#include "esp_err.h"
#include "esp_http_server.h"

/* 处理一次固件上传（从 httpd 请求体流式写入 OTA 分区）。
 * 成功返回 ESP_OK 并触发重启；失败返回非 OK（调用方回错误应答）。 */
esp_err_t ota_update_handle(httpd_req_t *req);
