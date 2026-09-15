/*
 * config_store.c - NVS 持久化实现
 */
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "config_store.h"

static const char *TAG = "cfg_store";

#define CFG_NAMESPACE "wifi_cfg"

#define KEY_SSID       "ssid"
#define KEY_PASS       "pass"
#define KEY_IP_MODE    "ip_mode"
#define KEY_IP         "ip"
#define KEY_NETMASK    "mask"
#define KEY_GATEWAY    "gw"
#define KEY_DNS        "dns"
#define KEY_AP_OFF     "ap_off"
#define KEY_AP_FB_DLY  "ap_fb_dly"
#define KEY_CONFIGURED "configured"

#define AP_FALLBACK_DELAY_DEFAULT 15

static void cfg_clear_struct(wifi_config_data_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    /* 默认值 */
    cfg->ip_mode = IP_MODE_DHCP;
    cfg->ap_off = true;
    cfg->ap_fallback_delay = AP_FALLBACK_DELAY_DEFAULT;
}

esp_err_t config_store_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

bool config_store_load(wifi_config_data_t *cfg)
{
    cfg_clear_struct(cfg);

    nvs_handle_t h;
    if (nvs_open(CFG_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }

    uint8_t configured = 0;
    esp_err_t ret = nvs_get_u8(h, KEY_CONFIGURED, &configured);
    if (ret != ESP_OK || configured == 0) {
        nvs_close(h);
        return false;
    }

    size_t len;
    len = sizeof(cfg->ssid);
    if (nvs_get_str(h, KEY_SSID, cfg->ssid, &len) != ESP_OK) {
        nvs_close(h);
        return false;
    }
    len = sizeof(cfg->password);
    if (nvs_get_str(h, KEY_PASS, cfg->password, &len) != ESP_OK) {
        cfg->password[0] = '\0';
    }

    uint8_t v = 0;
    if (nvs_get_u8(h, KEY_IP_MODE, &v) == ESP_OK) {
        cfg->ip_mode = v;
    }
    if (nvs_get_u8(h, KEY_AP_OFF, &v) == ESP_OK) {
        cfg->ap_off = (v != 0);
    }
    uint16_t u16 = 0;
    if (nvs_get_u16(h, KEY_AP_FB_DLY, &u16) == ESP_OK && u16 > 0) {
        cfg->ap_fallback_delay = u16;
    }

    if (cfg->ip_mode == IP_MODE_STATIC) {
        len = sizeof(cfg->ip);
        nvs_get_str(h, KEY_IP, cfg->ip, &len);
        len = sizeof(cfg->netmask);
        nvs_get_str(h, KEY_NETMASK, cfg->netmask, &len);
        len = sizeof(cfg->gateway);
        nvs_get_str(h, KEY_GATEWAY, cfg->gateway, &len);
        len = sizeof(cfg->dns);
        nvs_get_str(h, KEY_DNS, cfg->dns, &len);
    }

    cfg->configured = true;
    nvs_close(h);
    return true;
}

esp_err_t config_store_save(const wifi_config_data_t *cfg)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(CFG_NAMESPACE, NVS_READWRITE, &h);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_str(h, KEY_SSID, cfg->ssid);
    if (ret != ESP_OK) goto done;
    ret = nvs_set_str(h, KEY_PASS, cfg->password);
    if (ret != ESP_OK) goto done;
    ret = nvs_set_u8(h, KEY_IP_MODE, cfg->ip_mode);
    if (ret != ESP_OK) goto done;
    ret = nvs_set_u8(h, KEY_AP_OFF, cfg->ap_off ? 1 : 0);
    if (ret != ESP_OK) goto done;
    ret = nvs_set_u16(h, KEY_AP_FB_DLY, cfg->ap_fallback_delay);
    if (ret != ESP_OK) goto done;

    if (cfg->ip_mode == IP_MODE_STATIC) {
        ret = nvs_set_str(h, KEY_IP, cfg->ip);
        if (ret != ESP_OK) goto done;
        ret = nvs_set_str(h, KEY_NETMASK, cfg->netmask);
        if (ret != ESP_OK) goto done;
        ret = nvs_set_str(h, KEY_GATEWAY, cfg->gateway);
        if (ret != ESP_OK) goto done;
        ret = nvs_set_str(h, KEY_DNS, cfg->dns);
        if (ret != ESP_OK) goto done;
    } else {
        nvs_erase_key(h, KEY_IP);
        nvs_erase_key(h, KEY_NETMASK);
        nvs_erase_key(h, KEY_GATEWAY);
        nvs_erase_key(h, KEY_DNS);
    }

    ret = nvs_set_u8(h, KEY_CONFIGURED, 1);
    if (ret != ESP_OK) goto done;

    ret = nvs_commit(h);
done:
    nvs_close(h);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "save config failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t config_store_clear(void)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(CFG_NAMESPACE, NVS_READWRITE, &h);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_erase_all(h);
    if (ret == ESP_OK) {
        ret = nvs_commit(h);
    }
    nvs_close(h);
    return ret;
}

