/*
 * ota_update.c - 咖啡台 OTA 固件升级（网页上传）
 *
 * 流程：接收 HTTP 请求体（新固件二进制）→ esp_ota_begin 到另一个 app 分区
 *       → 分块写入 → esp_ota_end → 校验镜像 → 设置启动分区 → 重启。
 * 任一步失败：esp_ota_abort 回滚（启动分区不变，掉电也不会坏），并回错误应答。
 */
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_http_server.h"

#include "ota_update.h"

static const char *TAG = "ota";

#define OTA_BUF_SIZE 4096

esp_err_t ota_update_handle(httpd_req_t *req)
{
    /* 目标：当前运行分区之外的那个 app 分区 */
    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (!update) {
        ESP_LOGE(TAG, "no OTA partition found");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OTA target partition %s @ 0x%x size 0x%x",
             update->label, (unsigned)update->address, (unsigned)update->size);

    esp_ota_handle_t handle;
    esp_err_t err = esp_ota_begin(update, OTA_SIZE_UNKNOWN, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_begin failed");
        return err;
    }

    char *buf = malloc(OTA_BUF_SIZE);
    if (!buf) {
        esp_ota_abort(handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
        return ESP_ERR_NO_MEM;
    }

    int remaining = req->content_len;
    ESP_LOGI(TAG, "OTA receiving %d bytes", remaining);

    while (remaining > 0) {
        int to_read = remaining > OTA_BUF_SIZE ? OTA_BUF_SIZE : remaining;
        int r = httpd_req_recv(req, buf, to_read);
        if (r <= 0) {
            ESP_LOGE(TAG, "recv failed / connection closed: %d", r);
            free(buf);
            esp_ota_abort(handle);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body truncated");
            return ESP_FAIL;
        }
        err = esp_ota_write(handle, buf, r);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ota_write failed: %s", esp_err_to_name(err));
            free(buf);
            esp_ota_abort(handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_write failed");
            return err;
        }
        remaining -= r;
    }
    free(buf);

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        /* esp_ota_end 已自行收尾 handle，无需再 abort */
        ESP_LOGE(TAG, "ota_end (image invalid?) failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid image");
        return err;
    }

    /* 校验通过，设置下次从新分区启动 */
    err = esp_ota_set_boot_partition(update);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set boot failed");
        return err;
    }

    ESP_LOGI(TAG, "OTA success, rebooting to %s", update->label);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true,\"msg\":\"OTA success, rebooting\"}", HTTPD_RESP_USE_STRLEN);
    /* 延时让应答发出后再重启 */
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return ESP_OK;
}
