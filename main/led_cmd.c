/*
 * led_cmd.c - 咖啡台 LED 命令分发（JSON 解析 + 执行 + 组应答）
 *
 * 被 TCP(9001) 与 BLE GATT 共用，保证命令语义一致。
 * 支持命令：ping / led_set / led_set_all / led_set_batch / led_status。
 */
#include <string.h>
#include <stdlib.h>

#include "cJSON.h"

#include "led_cmd.h"
#include "led_control.h"

#define LED_MIN_CH  1
#define LED_MAX_CH  10

/* ---- 错误码 ---- */
#define E_INVALID_JSON   "INVALID_JSON"
#define E_INVALID_CMD    "INVALID_CMD"
#define E_INVALID_CH     "INVALID_CHANNEL"
#define E_INVALID_VALUE  "INVALID_VALUE"
#define E_INVALID_LIST   "INVALID_LIST"
#define E_INTERNAL       "INTERNAL"

/* 应答构造 */
static cJSON *resp_new(const cJSON *req, const char *cmd)
{
    cJSON *resp = cJSON_CreateObject();
    if (req) {
        const cJSON *seq = cJSON_GetObjectItem(req, "seq");
        if (cJSON_IsNumber(seq)) {
            cJSON_AddItemToObject(resp, "seq", cJSON_Duplicate(seq, 1));
        }
    }
    cJSON_AddStringToObject(resp, "cmd", cmd ? cmd : "");
    return resp;
}

static void resp_ok(cJSON *resp, cJSON *data)
{
    cJSON_AddBoolToObject(resp, "ok", true);
    if (data) {
        cJSON_AddItemToObject(resp, "data", data);
    }
}

static void resp_err(cJSON *resp, const char *err, const char *msg)
{
    cJSON_AddBoolToObject(resp, "ok", false);
    cJSON_AddStringToObject(resp, "err", err);
    cJSON_AddStringToObject(resp, "msg", msg ? msg : "");
}

/* 参数辅助 */
static bool get_int(const cJSON *obj, const char *key, int *out)
{
    const cJSON *j = cJSON_GetObjectItem(obj, key);
    if (!cJSON_IsNumber(j)) return false;
    *out = j->valueint;
    return true;
}

static bool get_bool(const cJSON *obj, const char *key, bool *out)
{
    const cJSON *j = cJSON_GetObjectItem(obj, key);
    if (!cJSON_IsBool(j)) return false;
    *out = cJSON_IsTrue(j);
    return true;
}

/* 各命令执行：成功返回 data；失败返回 NULL 并置 err[2]（err[0]=code, err[1]=msg） */
static cJSON *exec_ping(void)
{
    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "pong", true);
    return data;
}

static cJSON *exec_led_set(const cJSON *req, const char *err[2])
{
    int ch = 0;
    bool on = false;
    if (!get_int(req, "ch", &ch)) {
        err[0] = E_INVALID_CH; err[1] = "缺少 ch";
        return NULL;
    }
    if (ch < LED_MIN_CH || ch > LED_MAX_CH) {
        err[0] = E_INVALID_CH; err[1] = "ch 需为 1..10";
        return NULL;
    }
    if (!get_bool(req, "on", &on)) {
        err[0] = E_INVALID_VALUE; err[1] = "on 需为布尔";
        return NULL;
    }
    if (!led_set((uint8_t)ch, on)) {
        err[0] = E_INTERNAL; err[1] = "执行失败";
        return NULL;
    }
    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "ch", ch);
    cJSON_AddBoolToObject(data, "on", on);
    return data;
}

static cJSON *exec_led_set_all(const cJSON *req, const char *err[2])
{
    bool on = false;
    if (!get_bool(req, "on", &on)) {
        err[0] = E_INVALID_VALUE; err[1] = "on 需为布尔";
        return NULL;
    }
    int n = 0;
    for (int ch = LED_MIN_CH; ch <= LED_MAX_CH; ch++) {
        if (led_set((uint8_t)ch, on)) n++;
    }
    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "count", n);
    cJSON_AddBoolToObject(data, "on", on);
    return data;
}

static cJSON *exec_led_set_batch(const cJSON *req, const char *err[2])
{
    const cJSON *list = cJSON_GetObjectItem(req, "list");
    if (!cJSON_IsArray(list) || cJSON_GetArraySize(list) == 0) {
        err[0] = E_INVALID_LIST; err[1] = "list 需为非空数组";
        return NULL;
    }
    int n = cJSON_GetArraySize(list);
    if (n > LED_MAX_CH) {
        err[0] = E_INVALID_LIST; err[1] = "list 最多 10 条";
        return NULL;
    }
    int *chs = malloc(sizeof(int) * n);
    bool *ons = malloc(sizeof(bool) * n);
    if (!chs || !ons) {
        free(chs); free(ons);
        err[0] = E_INTERNAL; err[1] = "内存不足";
        return NULL;
    }
    for (int i = 0; i < n; i++) {
        const cJSON *item = cJSON_GetArrayItem(list, i);
        int ch = 0;
        bool on = false;
        if (!cJSON_IsObject(item) || !get_int(item, "ch", &ch) || !get_bool(item, "on", &on)) {
            free(chs); free(ons);
            err[0] = E_INVALID_LIST; err[1] = "list 元素需为 {ch,on}";
            return NULL;
        }
        if (ch < LED_MIN_CH || ch > LED_MAX_CH) {
            free(chs); free(ons);
            err[0] = E_INVALID_CH; err[1] = "ch 需为 1..10";
            return NULL;
        }
        chs[i] = ch;
        ons[i] = on;
    }
    int applied = 0;
    for (int i = 0; i < n; i++) {
        if (led_set((uint8_t)chs[i], ons[i])) applied++;
    }
    free(chs); free(ons);
    cJSON *data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "applied", applied);
    return data;
}

static cJSON *exec_led_status(void)
{
    cJSON *channels = cJSON_CreateArray();
    for (int ch = LED_MIN_CH; ch <= LED_MAX_CH; ch++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "ch", ch);
        cJSON_AddBoolToObject(item, "on", led_get((uint8_t)ch));
        cJSON_AddItemToArray(channels, item);
    }
    cJSON *data = cJSON_CreateObject();
    cJSON_AddItemToObject(data, "channels", channels);
    return data;
}

/* ------------------------------------------------------------------ */
/* 对外：解析一行 JSON，执行并返回应答 JSON 字符串                        */
/* ------------------------------------------------------------------ */
char *led_cmd_execute(const char *json)
{
    cJSON *req = cJSON_Parse(json);
    if (!req) {
        cJSON *resp = resp_new(NULL, "");
        resp_err(resp, E_INVALID_JSON, "JSON 解析失败");
        char *out = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        return out;
    }

    const cJSON *cmd_j = cJSON_GetObjectItem(req, "cmd");
    const char *cmd = cJSON_IsString(cmd_j) ? cmd_j->valuestring : "";
    cJSON *resp = resp_new(req, cmd);

    cJSON *data = NULL;
    const char *err[2] = {NULL, NULL};

    if (strcmp(cmd, "ping") == 0) {
        data = exec_ping();
    } else if (strcmp(cmd, "led_set") == 0) {
        data = exec_led_set(req, err);
    } else if (strcmp(cmd, "led_set_all") == 0) {
        data = exec_led_set_all(req, err);
    } else if (strcmp(cmd, "led_set_batch") == 0) {
        data = exec_led_set_batch(req, err);
    } else if (strcmp(cmd, "led_status") == 0) {
        data = exec_led_status();
    } else {
        err[0] = E_INVALID_CMD;
        err[1] = "未知命令";
    }

    if (data) {
        resp_ok(resp, data);
    } else {
        resp_err(resp, err[0] ? err[0] : E_INVALID_CMD, err[1]);
    }

    char *out = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    cJSON_Delete(req);
    return out;
}
