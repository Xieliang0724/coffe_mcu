/*
 * ble_led.c - 咖啡台 BLE GATT 控制服务（NimBLE）
 *
 * 服务/特征（基于 Bluetooth base UUID，16-bit 自定义）：
 *   服务           0xFFE0
 *   CMD  (Write)   0xFFE1   App 写入一条 JSON 命令
 *   RESP (Notify)  0xFFE2   设备回传应答 JSON（App 订阅通知后收到）
 *   STA  (Read)    0xFFE3   读当前全部状态 JSON
 *
 * 命令语义与 TCP(9001) 一致（复用 led_cmd）。与 WiFi / Modbus(502) 共存。
 */
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_hs_id.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "ble_led.h"
#include "led_cmd.h"

static const char *TAG = "ble_ctrl";

/* ----- GATT UUID ----- */
#define SERVICE_UUID  0xFFE0
#define CMD_UUID      0xFFE1
#define RESP_UUID     0xFFE2
#define STA_UUID      0xFFE3

static uint8_t s_own_addr_type = BLE_OWN_ADDR_PUBLIC;

/* Notify 所需：最近的订阅连接 + RESP 特征值句柄 */
static uint16_t s_resp_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_resp_attr = 0;
static char s_last_resp[512] = {0};

static void start_advertising(void);

/* ------------------------------------------------------------------ */
/* 通知                                                               */
/* ------------------------------------------------------------------ */

static void notify_resp(void)
{
    if (s_resp_conn == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(s_last_resp, strlen(s_last_resp));
    if (om) {
        ble_gatts_notify_custom(s_resp_conn, s_resp_attr, om);
    }
}

/* ------------------------------------------------------------------ */
/* 特征访问回调                                                        */
/* ------------------------------------------------------------------ */

/* CMD (Write)：执行命令并通知应答 */
static int gatt_write_cmd(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len == 0 || len >= sizeof(s_last_resp)) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    char cmd[sizeof(s_last_resp)];
    os_mbuf_copydata(ctxt->om, 0, len, cmd);
    cmd[len] = '\0';
    ESP_LOGI(TAG, "BLE cmd: %s", cmd);

    char *resp = led_cmd_execute(cmd);
    if (resp) {
        strncpy(s_last_resp, resp, sizeof(s_last_resp) - 1);
        s_last_resp[sizeof(s_last_resp) - 1] = '\0';
        notify_resp();
        free(resp);
    }
    return 0;
}

/* RESP (Read)：返回最近一次应答（未发过则为空） */
static int gatt_read_resp(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;
    int rc = os_mbuf_append(ctxt->om, s_last_resp, strlen(s_last_resp));
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

/* STA (Read)：返回当前全部状态 */
static int gatt_read_sta(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;
    char *resp = led_cmd_execute("{\"cmd\":\"led_status\"}");
    if (!resp) {
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    int rc = os_mbuf_append(ctxt->om, resp, strlen(resp));
    free(resp);
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(SERVICE_UUID),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(CMD_UUID),
                .access_cb = gatt_write_cmd,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = BLE_UUID16_DECLARE(RESP_UUID),
                .access_cb = gatt_read_resp,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid = BLE_UUID16_DECLARE(STA_UUID),
                .access_cb = gatt_read_sta,
                .flags = BLE_GATT_CHR_F_READ,
            },
            { 0 }
        }
    },
    { 0 }
};

/* ------------------------------------------------------------------ */
/* GAP 事件                                                            */
/* ------------------------------------------------------------------ */

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "BLE connect status=%d", event->connect.status);
        if (event->connect.status != 0) {
            s_resp_conn = BLE_HS_CONN_HANDLE_NONE;
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE disconnect");
        s_resp_conn = BLE_HS_CONN_HANDLE_NONE;
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        /* RESP 特征被订阅/取消订阅：记录或清理通知目标连接 */
        if (event->subscribe.cur_notify || event->subscribe.cur_indicate) {
            s_resp_conn = event->subscribe.conn_handle;
            ESP_LOGI(TAG, "RESP subscribed conn=%u", s_resp_conn);
        } else if (event->subscribe.conn_handle == s_resp_conn) {
            s_resp_conn = BLE_HS_CONN_HANDLE_NONE;
            ESP_LOGI(TAG, "RESP unsubscribed");
        }
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        start_advertising();
        break;

    default:
        break;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 广播                                                               */
/* ------------------------------------------------------------------ */

static void start_advertising(void)
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    const char *name = ble_svc_gap_device_name();
    fields.name = (const uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv set fields failed: %d", rc);
        return;
    }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv start failed: %d", rc);
    }
}

/* ------------------------------------------------------------------ */
/* 主机任务                                                            */
/* ------------------------------------------------------------------ */

static void nimble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* 协议栈同步后就绪 */
static void on_sync(int reason)
{
    (void)reason;
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure addr failed: %d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer addr failed: %d", rc);
        return;
    }
    /* host 同步完成，GATT 表此时才就绪：解析 RESP 特征值句柄用于 notify */
    uint16_t def_handle = 0;
    int rc2 = ble_gatts_find_chr(BLE_UUID16_DECLARE(SERVICE_UUID), BLE_UUID16_DECLARE(RESP_UUID),
                                 &def_handle, &s_resp_attr);
    if (rc2 != 0) {
        ESP_LOGW(TAG, "find RESP chr failed (notify disabled): %d", rc2);
        s_resp_attr = 0;
    }
    start_advertising();
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

esp_err_t ble_led_init(void)
{
    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();

    /* 设备名（广播名） */
    const char *name = "CoffeeTable-LED";
    ble_svc_gap_device_name_set(name);

    /* 注册 GATT 服务 */
    int rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "count cfg failed: %d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "add svcs failed: %d", rc);
        return ESP_FAIL;
    }

    /* 回调必须在 host 任务启动前挂好，否则 sync 可能错过 */
    ble_hs_cfg.reset_cb = on_sync;

    nimble_port_freertos_init(nimble_host_task);

    ESP_LOGI(TAG, "BLE GATT started, service 0xFFE0, name=%s", name);
    return ESP_OK;
}
