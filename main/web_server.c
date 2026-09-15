/*
 * web_server.c - Web 配网服务器实现
 */
#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_app_desc.h"
#include "cJSON.h"

#include "config_store.h"
#include "led_control.h"
#include "web_server.h"
#include "wifi_mgr.h"

static const char *TAG = "web_srv";

/* 内嵌网页 (main/www/index.html) */
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

#define MAX_POST_BODY 1024
#define SCAN_LIST_MAX 30

static httpd_handle_t s_server = NULL;
static bool s_scan_done = false;

/* 延迟操作定时器：静态复用（懒创建），避免每次请求创建后泄漏 */
static esp_timer_handle_t s_enter_config_timer = NULL;
static esp_timer_handle_t s_connect_timer = NULL;
static wifi_config_data_t s_pending_cfg;    /* 最新待应用的配网配置（新请求覆盖旧的） */

static void arm_enter_config_timer(void);
static void arm_connect_timer(void);

static const char *AUTH_NAMES[] = {
    [WIFI_AUTH_OPEN]         = "OPEN",
    [WIFI_AUTH_WEP]          = "WEP",
    [WIFI_AUTH_WPA_PSK]      = "WPA_PSK",
    [WIFI_AUTH_WPA2_PSK]     = "WPA2_PSK",
    [WIFI_AUTH_WPA_WPA2_PSK] = "WPA_WPA2_PSK",
    [WIFI_AUTH_WPA3_PSK]     = "WPA3_PSK",
    [WIFI_AUTH_WPA2_WPA3_PSK]= "WPA2_WPA3_PSK",
    [WIFI_AUTH_OWE]          = "OWE",
};

static void scan_done_cb(void)
{
    s_scan_done = true;
}

static const char *auth_name(wifi_auth_mode_t m)
{
    if (m < sizeof(AUTH_NAMES) / sizeof(AUTH_NAMES[0]) && AUTH_NAMES[m]) {
        return AUTH_NAMES[m];
    }
    return "UNKNOWN";
}

/* ------------------------------------------------------------------ */
/* 工具                                                                 */
/* ------------------------------------------------------------------ */

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t send_json_obj(httpd_req_t *req, cJSON *obj)
{
    char *s = cJSON_PrintUnformatted(obj);
    if (!s) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret = send_json(req, s);
    free(s);
    return ret;
}

/* 读取 POST body（JSON）到 buf */
static esp_err_t recv_body(httpd_req_t *req, char *buf, size_t buf_size)
{
    if (req->content_len >= buf_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    int total = 0;
    while (total < req->content_len) {
        int r = httpd_req_recv(req, buf + total, req->content_len - total);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return ESP_FAIL;
        }
        total += r;
    }
    buf[total] = '\0';
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 路由                                                                 */
/* ------------------------------------------------------------------ */

static esp_err_t handle_root(httpd_req_t *req)
{
    size_t len = index_html_end - index_html_start;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)index_html_start, len);
}

static esp_err_t handle_status(httpd_req_t *req)
{
    wifi_mgr_state_t st = wifi_mgr_get_state();
    const char *state_str = "unknown";
    switch (st) {
    case WIFI_MGR_STATE_CONFIG:     state_str = "config"; break;
    case WIFI_MGR_STATE_CONNECTING: state_str = "connecting"; break;
    case WIFI_MGR_STATE_CONNECTED:  state_str = "connected"; break;
    default: break;
    }

    /* 当前 STA 配置的 SSID */
    char ssid[33] = {0};
    wifi_config_t wcfg = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &wcfg) == ESP_OK) {
        strlcpy(ssid, (char *)wcfg.sta.ssid, sizeof(ssid));
    }
    char ip[32] = {0};
    wifi_mgr_get_sta_ip(ip, sizeof(ip));

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "state", state_str);
    cJSON_AddStringToObject(obj, "ssid", ssid);
    cJSON_AddStringToObject(obj, "ip", ip);
    cJSON_AddBoolToObject(obj, "ap_on", wifi_mgr_is_ap_on());

    /* STA 网络信息：网关/掩码/DNS */
    char gw[32] = {0}, mask[32] = {0}, dns[32] = {0};
    esp_netif_t *sta = wifi_mgr_get_sta_netif();
    if (sta) {
        esp_netif_ip_info_t ip_info = {0};
        if (esp_netif_get_ip_info(sta, &ip_info) == ESP_OK) {
            esp_ip4addr_ntoa(&ip_info.gw, gw, sizeof(gw));
            esp_ip4addr_ntoa(&ip_info.netmask, mask, sizeof(mask));
        }
        esp_netif_dns_info_t dns_info = {0};
        if (esp_netif_get_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns_info) == ESP_OK) {
            esp_ip4addr_ntoa(&dns_info.ip.u_addr.ip4, dns, sizeof(dns));
        }
    }
    cJSON_AddStringToObject(obj, "gateway", gw);
    cJSON_AddStringToObject(obj, "netmask", mask);
    cJSON_AddStringToObject(obj, "dns", dns);

    /* 信号强度（未连接时 rssi=0） */
    int8_t rssi = 0;
    wifi_ap_record_t ap_info = {0};
    if (wifi_mgr_get_sta_ap_info(&ap_info) == ESP_OK) {
        rssi = ap_info.rssi;
    }
    cJSON_AddNumberToObject(obj, "rssi", rssi);

    /* AP 信息 */
    char ap_ssid[33] = {0};
    wifi_mgr_get_ap_ssid(ap_ssid, sizeof(ap_ssid));
    cJSON_AddStringToObject(obj, "ap_ssid", ap_ssid);
    cJSON_AddNumberToObject(obj, "ap_clients", wifi_mgr_get_ap_clients());

    /* 当前 Wi-Fi 模式 */
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    cJSON_AddStringToObject(obj, "wifi_mode",
                            mode == WIFI_MODE_STA ? "STA" :
                            mode == WIFI_MODE_AP ? "AP" :
                            mode == WIFI_MODE_APSTA ? "APSTA" : "NULL");

    /* 固件版本（来自 git tag / PROJECT_VER） */
    const esp_app_desc_t *app = esp_app_get_description();
    cJSON_AddStringToObject(obj, "version", app ? app->version : "unknown");

    esp_err_t ret = send_json_obj(req, obj);
    cJSON_Delete(obj);
    return ret;
}

/* 运行时开关 SoftAP */
static esp_err_t handle_ap_post(httpd_req_t *req)
{
    char buf[MAX_POST_BODY];
    esp_err_t ret = recv_body(req, buf, sizeof(buf));
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ret;
    }
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
        return ESP_FAIL;
    }
    cJSON *j = cJSON_GetObjectItem(root, "ap_on");
    bool want_on = cJSON_IsBool(j) && cJSON_IsTrue(j);
    cJSON_Delete(root);

    esp_err_t r = want_on ? wifi_mgr_ap_enable() : wifi_mgr_ap_disable();
    cJSON *obj = cJSON_CreateObject();
    if (r == ESP_OK) {
        cJSON_AddStringToObject(obj, "status", "ok");
        cJSON_AddBoolToObject(obj, "ap_on", wifi_mgr_is_ap_on());
    } else {
        cJSON_AddStringToObject(obj, "status", "error");
        cJSON_AddStringToObject(obj, "msg",
            want_on ? esp_err_to_name(r) : "STA 未连接，关闭 AP 将导致设备失联");
    }
    ret = send_json_obj(req, obj);
    cJSON_Delete(obj);
    return ret;
}

/* 断开 STA 并进入配网模式（不删除已保存配置，重启后仍按原配置联网） */
static esp_err_t handle_disconnect_post(httpd_req_t *req)
{
    cJSON *ok = cJSON_CreateObject();
    cJSON_AddStringToObject(ok, "status", "ok");
    esp_err_t ret = send_json_obj(req, ok);   /* 必须先响应，STA 断开后网页将失联 */
    cJSON_Delete(ok);

    /* 延迟执行：等响应发出后再切换模式 */
    arm_enter_config_timer();
    return ret;
}

static esp_err_t handle_scan(httpd_req_t *req)
{
    cJSON *obj = cJSON_CreateObject();

    /* 方案B：仅配网模式允许扫描，避免抢占射频影响 STA 业务/连接 */
    if (wifi_mgr_get_state() != WIFI_MGR_STATE_CONFIG) {
        cJSON_AddStringToObject(obj, "status", "error");
        cJSON_AddStringToObject(obj, "msg", "STA 已联网/连接中，扫描已禁用以保障业务");
        esp_err_t ret = send_json_obj(req, obj);
        cJSON_Delete(obj);
        return ret;
    }

    if (s_scan_done) {
        s_scan_done = false;

        wifi_ap_record_t *records = NULL;
        uint16_t count = 0;
        esp_err_t ret = wifi_mgr_scan_get_results(&records, &count);
        if (ret != ESP_OK) {
            cJSON_AddStringToObject(obj, "status", "error");
            cJSON_AddStringToObject(obj, "msg", esp_err_to_name(ret));
        } else {
            cJSON *nets = cJSON_AddArrayToObject(obj, "networks");
            uint16_t shown = 0;
            for (uint16_t i = 0; i < count && shown < SCAN_LIST_MAX; i++) {
                if (records[i].ssid[0] == '\0') {
                    continue;   /* 隐藏 SSID */
                }
                cJSON *n = cJSON_CreateObject();
                cJSON_AddStringToObject(n, "ssid", (char *)records[i].ssid);
                cJSON_AddNumberToObject(n, "rssi", records[i].rssi);
                cJSON_AddStringToObject(n, "auth", auth_name(records[i].authmode));
                cJSON_AddNumberToObject(n, "channel", records[i].primary);
                cJSON_AddStringToObject(n, "band", wifi_mgr_band_of_channel(records[i].primary));
                cJSON_AddItemToArray(nets, n);
                shown++;
            }
            cJSON_AddStringToObject(obj, "status", "ok");
            if (records) {
                free(records);
            }
        }
    } else if (!wifi_mgr_is_scanning()) {
        /* 启动一次新扫描 */
        esp_err_t ret = wifi_mgr_scan_async(SCAN_LIST_MAX, scan_done_cb);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            cJSON_AddStringToObject(obj, "status", "error");
            cJSON_AddStringToObject(obj, "msg", esp_err_to_name(ret));
        } else {
            cJSON_AddStringToObject(obj, "status", "scanning");
        }
    } else {
        cJSON_AddStringToObject(obj, "status", "scanning");
    }

    esp_err_t ret = send_json_obj(req, obj);
    cJSON_Delete(obj);
    return ret;
}

/* 延迟执行连接/复位，避免在 HTTP handler 内部重启 Wi-Fi/停服务器 */
static void enter_config_cb(void *arg)
{
    wifi_mgr_enter_config_mode();
}

static void connect_cb(void *arg)
{
    wifi_mgr_connect(&s_pending_cfg);
}

static void arm_enter_config_timer(void)
{
    if (!s_enter_config_timer) {
        esp_timer_create_args_t targs = {
            .callback = enter_config_cb,
            .name = "delayed_config",
        };
        if (esp_timer_create(&targs, &s_enter_config_timer) != ESP_OK) {
            ESP_LOGE(TAG, "create delayed_config timer failed");
            return;
        }
    }
    esp_timer_stop(s_enter_config_timer);
    esp_timer_start_once(s_enter_config_timer, 300 * 1000);
}

static void arm_connect_timer(void)
{
    if (!s_connect_timer) {
        esp_timer_create_args_t targs = {
            .callback = connect_cb,
            .name = "delayed_connect",
        };
        if (esp_timer_create(&targs, &s_connect_timer) != ESP_OK) {
            ESP_LOGE(TAG, "create delayed_connect timer failed");
            return;
        }
    }
    esp_timer_stop(s_connect_timer);
    esp_timer_start_once(s_connect_timer, 300 * 1000);
}

static esp_err_t handle_config_post(httpd_req_t *req)
{
    char buf[MAX_POST_BODY];
    esp_err_t ret = recv_body(req, buf, sizeof(buf));
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ret;
    }

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
        return ESP_FAIL;
    }

    cJSON *j_ssid = cJSON_GetObjectItem(root, "ssid");
    if (!cJSON_IsString(j_ssid) || strlen(j_ssid->valuestring) == 0 ||
        strlen(j_ssid->valuestring) >= CFG_SSID_MAX_LEN) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
        return ESP_FAIL;
    }

    wifi_config_data_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strlcpy(cfg.ssid, j_ssid->valuestring, sizeof(cfg.ssid));

    cJSON *j_pass = cJSON_GetObjectItem(root, "password");
    if (cJSON_IsString(j_pass) && strlen(j_pass->valuestring) < CFG_PASS_MAX_LEN) {
        strlcpy(cfg.password, j_pass->valuestring, sizeof(cfg.password));
    }

    cJSON *j_mode = cJSON_GetObjectItem(root, "ip_mode");
    if (cJSON_IsString(j_mode) && strcmp(j_mode->valuestring, "static") == 0) {
        cfg.ip_mode = IP_MODE_STATIC;
    }

    if (cfg.ip_mode == IP_MODE_STATIC) {
        cJSON *j_ip = cJSON_GetObjectItem(root, "ip");
        cJSON *j_gw = cJSON_GetObjectItem(root, "gateway");
        if (!cJSON_IsString(j_ip) || strlen(j_ip->valuestring) == 0 ||
            !cJSON_IsString(j_gw) || strlen(j_gw->valuestring) == 0) {
            cJSON_Delete(root);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "static mode needs ip & gateway");
            return ESP_FAIL;
        }
        strlcpy(cfg.ip, j_ip->valuestring, sizeof(cfg.ip));
        strlcpy(cfg.gateway, j_gw->valuestring, sizeof(cfg.gateway));
        cJSON *j_mask = cJSON_GetObjectItem(root, "netmask");
        if (cJSON_IsString(j_mask)) {
            strlcpy(cfg.netmask, j_mask->valuestring, sizeof(cfg.netmask));
        }
        cJSON *j_dns = cJSON_GetObjectItem(root, "dns");
        if (cJSON_IsString(j_dns)) {
            strlcpy(cfg.dns, j_dns->valuestring, sizeof(cfg.dns));
        }
    }

    cJSON *j_ap_off = cJSON_GetObjectItem(root, "ap_off");
    cfg.ap_off = (cJSON_IsBool(j_ap_off) && cJSON_IsTrue(j_ap_off)) ? true : false;

    cJSON *j_fb = cJSON_GetObjectItem(root, "ap_fallback_delay");
    if (cJSON_IsNumber(j_fb) && j_fb->valueint >= 0 && j_fb->valueint <= 3600) {
        cfg.ap_fallback_delay = j_fb->valueint;
    }

    cJSON_Delete(root);

    ret = config_store_save(&cfg);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ret;
    }

    cJSON *ok = cJSON_CreateObject();
    cJSON_AddStringToObject(ok, "status", "ok");
    ret = send_json_obj(req, ok);
    cJSON_Delete(ok);

    /* 响应已发出，延迟 300ms 再连接（避免从 handler 内部停掉服务器） */
    memcpy(&s_pending_cfg, &cfg, sizeof(cfg));
    arm_connect_timer();
    return ret;
}

static esp_err_t handle_reset_post(httpd_req_t *req)
{
    config_store_clear();
    cJSON *ok = cJSON_CreateObject();
    cJSON_AddStringToObject(ok, "status", "ok");
    esp_err_t ret = send_json_obj(req, ok);
    cJSON_Delete(ok);

    /* 延迟进入配网模式 */
    arm_enter_config_timer();
    return ret;
}

/* ------------------------------------------------------------------ */
/* 咖啡台 LED 控制                                                      */
/* ------------------------------------------------------------------ */

/* GET /api/leds -> 返回 10 路开关状态 */
static esp_err_t handle_leds_get(httpd_req_t *req)
{
    cJSON *arr = cJSON_CreateArray();
    for (int ch = 1; ch <= led_channel_count(); ch++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "channel", ch);
        cJSON_AddBoolToObject(item, "on", led_get((uint8_t)ch));
        cJSON_AddItemToArray(arr, item);
    }
    esp_err_t ret = send_json_obj(req, arr);
    cJSON_Delete(arr);
    return ret;
}

/* POST /api/led  body: {"channel":1,"on":true} -> 控制单路开关 */
static esp_err_t handle_led_post(httpd_req_t *req)
{
    char buf[MAX_POST_BODY];
    esp_err_t ret = recv_body(req, buf, sizeof(buf));
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
        return ESP_FAIL;
    }
    cJSON *ch_j = cJSON_GetObjectItem(root, "channel");
    cJSON *on_j = cJSON_GetObjectItem(root, "on");
    if (!cJSON_IsNumber(ch_j) || !cJSON_IsBool(on_j)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need channel(int) & on(bool)");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    uint8_t ch = (uint8_t)cJSON_GetNumberValue(ch_j);
    bool on = cJSON_IsTrue(on_j);
    bool ok = led_set(ch, on);
    cJSON_Delete(root);

    if (!ok) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad channel");
        return ESP_FAIL;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "channel", ch);
    cJSON_AddBoolToObject(resp, "on", on);
    ret = send_json_obj(req, resp);
    cJSON_Delete(resp);
    return ret;
}

/* ------------------------------------------------------------------ */
/* 服务器生命周期                                                       */
/* ------------------------------------------------------------------ */

static esp_err_t start_httpd(void)
{
    if (s_server) {
        return ESP_OK;   /* 幂等 */
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 16;
    return httpd_start(&s_server, &cfg);
}

static esp_err_t register_handlers(httpd_handle_t server)
{
    static const httpd_uri_t uris[] = {
        { .uri = "/",             .method = HTTP_GET,  .handler = handle_root },
        { .uri = "/api/status",   .method = HTTP_GET,  .handler = handle_status },
        { .uri = "/api/scan",     .method = HTTP_GET,  .handler = handle_scan },
        { .uri = "/api/config",   .method = HTTP_POST, .handler = handle_config_post },
        { .uri = "/api/reset",    .method = HTTP_POST, .handler = handle_reset_post },
        { .uri = "/api/ap",       .method = HTTP_POST, .handler = handle_ap_post },
        { .uri = "/api/disconnect", .method = HTTP_POST, .handler = handle_disconnect_post },
        { .uri = "/api/leds",     .method = HTTP_GET,  .handler = handle_leds_get },
        { .uri = "/api/led",      .method = HTTP_POST, .handler = handle_led_post },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        esp_err_t ret = httpd_register_uri_handler(server, &uris[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "register %s failed: %s", uris[i].uri, esp_err_to_name(ret));
            return ret;
        }
    }
    return ESP_OK;
}

esp_err_t web_server_start(void)
{
    if (s_server) {
        return ESP_OK;
    }
    esp_err_t ret = start_httpd();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = register_handlers(s_server);
    if (ret != ESP_OK) {
        httpd_stop(s_server);
        s_server = NULL;
        return ret;
    }
    ESP_LOGI(TAG, "web server started");
    return ESP_OK;
}

esp_err_t web_server_stop(void)
{
    if (!s_server) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "web server stopped");
    httpd_stop(s_server);
    s_server = NULL;
    return ESP_OK;
}

bool web_server_is_running(void)
{
    return s_server != NULL;
}
