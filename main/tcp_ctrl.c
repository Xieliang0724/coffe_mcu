/*
 * tcp_ctrl.c - 个性化 TCP LED 控制服务（JSON-over-TCP）
 *
 * 监听固定端口 9001，按 docs/2026-09-15_LED控制TCP协议_v1.0.md 处理：
 *   每条命令 = 单行 JSON + '\n'；执行完回一个 JSON + '\n'。
 * 支持命令：ping / led_set / led_set_all / led_set_batch / led_status。
 *
 * 纯网络实现（lwIP socket），不占用任何串口。
 */
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"
#include "cJSON.h"

#include "tcp_ctrl.h"
#include "led_control.h"

static const char *TAG = "tcp_ctrl";

#define CTRL_PORT       9001
#define MAX_CLIENTS     4
#define MAX_LINE        1024         /* 单条命令最大长度（含 '\n'），超长本行丢弃并回错 */
#define CLIENT_IDLE_MS  300000       /* 客户端空闲超时：300s 无数据则关闭，释放槽位 */
#define LED_MIN_CH      1
#define LED_MAX_CH      10

/* ---- 错误码 ---- */
#define E_INVALID_JSON   "INVALID_JSON"
#define E_INVALID_CMD    "INVALID_CMD"
#define E_INVALID_CH     "INVALID_CHANNEL"
#define E_INVALID_VALUE  "INVALID_VALUE"
#define E_INVALID_LIST   "INVALID_LIST"
#define E_INTERNAL       "INTERNAL"

static bool s_running = false;
static SemaphoreHandle_t s_slot_mutex = NULL;
static int s_client_fds[MAX_CLIENTS] = {-1, -1, -1, -1};
static TaskHandle_t s_client_tasks[MAX_CLIENTS] = {NULL};
static TaskHandle_t s_listen_task = NULL;
static int s_listen_fd = -1;

/* ------------------------------------------------------------------ */
/* 应答构造                                                            */
/* ------------------------------------------------------------------ */

/* 带 seq/cmd 的基础应答对象 */
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

/* 发送一行 JSON + '\n' */
static void send_line(int fd, cJSON *obj)
{
    char *s = cJSON_PrintUnformatted(obj);
    if (!s) {
        return;
    }
    size_t len = strlen(s);
    const char *nl = "\n";
    size_t total = len + 1;
    size_t sent = 0;
    while (sent < total && s_running) {
        const char *chunk = (sent < len) ? (s + sent) : nl;
        size_t chunk_len = (sent < len) ? (len - sent) : 1;
        int n = send(fd, chunk, chunk_len, 0);
        if (n <= 0) break;
        sent += n;
    }
    cJSON_free(s);
}

/* ------------------------------------------------------------------ */
/* 参数辅助                                                            */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/* 各命令执行：成功返回 data 对象；参数非法返回 NULL 并置 err_out       */
/* ------------------------------------------------------------------ */

/* ping */
static cJSON *exec_ping(void)
{
    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "pong", true);
    return data;
}

/* led_set {ch, on} */
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

/* led_set_all {on} */
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

/* led_set_batch {list:[{ch,on},...]} */
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
        if (!cJSON_IsObject(item) || !get_int(item, "ch", &ch) ||
            !get_bool(item, "on", &on)) {
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

/* led_status */
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
/* 请求分发：解析一行、执行、组应答                                     */
/* ------------------------------------------------------------------ */

static void handle_line(int fd, const char *line)
{
    cJSON *req = cJSON_Parse(line);
    cJSON *resp;

    if (!req) {
        resp = resp_new(NULL, "");
        resp_err(resp, E_INVALID_JSON, "JSON 解析失败");
        send_line(fd, resp);
        cJSON_Delete(resp);
        return;
    }

    const cJSON *cmd_j = cJSON_GetObjectItem(req, "cmd");
    const char *cmd = cJSON_IsString(cmd_j) ? cmd_j->valuestring : "";
    resp = resp_new(req, cmd);

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

    send_line(fd, resp);
    cJSON_Delete(resp);
    cJSON_Delete(req);
}

/* ------------------------------------------------------------------ */
/* 客户端任务：按 '\n' 切分单行 JSON                                    */
/* ------------------------------------------------------------------ */

static void close_client(int idx)
{
    if (idx >= 0 && idx < MAX_CLIENTS && s_client_fds[idx] >= 0) {
        close(s_client_fds[idx]);
        s_client_fds[idx] = -1;
        s_client_tasks[idx] = NULL;
    }
}

static int find_free_client_slot(void)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s_client_fds[i] < 0) {
            return i;
        }
    }
    return -1;
}

typedef struct {
    int fd;
    int slot;
} client_arg_t;

static void client_task(void *arg)
{
    client_arg_t *ca = (client_arg_t *)arg;
    int fd = ca->fd;
    int slot = ca->slot;
    free(ca);

    char buf[MAX_LINE];
    size_t len = 0;

    while (s_running) {
        char tmp[MAX_LINE];
        int n = recv(fd, tmp, sizeof(tmp) - 1, 0);
        if (n < 0) {
            /* SO_RCVTIMEO 触发的空闲超时：关闭连接并释放槽位 */
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                ESP_LOGI(TAG, "client idle timeout, close");
            }
            goto done;
        }
        if (n == 0) {
            goto done;   /* 对端正常关闭 */
        }
        for (int i = 0; i < n; i++) {
            char c = tmp[i];
            if (c == '\n') {
                buf[len] = '\0';
                if (len > 0) {
                    handle_line(fd, buf);
                }
                len = 0;
            } else if (len < sizeof(buf) - 1) {
                buf[len++] = c;
            } else {
                /* 单条超长，丢弃本行并回错误 */
                buf[len] = '\0';
                cJSON *resp = resp_new(NULL, "");
                resp_err(resp, E_INVALID_JSON, "命令过长");
                send_line(fd, resp);
                cJSON_Delete(resp);
                len = 0;
            }
        }
    }

done:
    ESP_LOGI(TAG, "client disconnected");
    xSemaphoreTake(s_slot_mutex, portMAX_DELAY);
    if (slot >= 0 && slot < MAX_CLIENTS && s_client_fds[slot] == fd) {
        close_client(slot);
    }
    xSemaphoreGive(s_slot_mutex);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* 监听任务                                                            */
/* ------------------------------------------------------------------ */

static void listen_task(void *arg)
{
    (void)arg;
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        ESP_LOGE(TAG, "socket failed");
        vTaskDelete(NULL);
        return;
    }
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    addr.sin_port = htons(CTRL_PORT);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind %u failed", CTRL_PORT);
        close(fd);
        vTaskDelete(NULL);
        return;
    }
    listen(fd, 4);
    s_listen_fd = fd;
    ESP_LOGI(TAG, "TCP LED control listening on port %u", CTRL_PORT);

    while (s_running) {
        int client = accept(fd, NULL, NULL);
        if (client < 0) {
            if (!s_running) break;
            continue;
        }
        /* 客户端空闲超时：60s 无数据自动关闭，释放槽位，防死连接占满 */
        struct timeval tv = { .tv_sec = CLIENT_IDLE_MS / 1000, .tv_usec = 0 };
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        xSemaphoreTake(s_slot_mutex, portMAX_DELAY);
        if (!s_running) {
            xSemaphoreGive(s_slot_mutex);
            close(client);
            break;
        }
        int slot = find_free_client_slot();
        if (slot >= 0) {
            s_client_fds[slot] = client;
        }
        xSemaphoreGive(s_slot_mutex);
        if (slot < 0) {
            ESP_LOGW(TAG, "too many clients, reject");
            close(client);
            continue;
        }
        client_arg_t *ca = malloc(sizeof(client_arg_t));
        if (!ca) {
            xSemaphoreTake(s_slot_mutex, portMAX_DELAY);
            s_client_fds[slot] = -1;
            s_client_tasks[slot] = NULL;
            xSemaphoreGive(s_slot_mutex);
            close(client);
            continue;
        }
        ca->fd = client;
        ca->slot = slot;
        if (xTaskCreate(client_task, "tcp_ctrl_cli", 6144, ca, 6,
                        &s_client_tasks[slot]) != pdPASS) {
            xSemaphoreTake(s_slot_mutex, portMAX_DELAY);
            s_client_fds[slot] = -1;
            s_client_tasks[slot] = NULL;
            xSemaphoreGive(s_slot_mutex);
            free(ca);
            close(client);
            continue;
        }
    }

    s_listen_fd = -1;
    close(fd);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                            */
/* ------------------------------------------------------------------ */

esp_err_t tcp_ctrl_init(void)
{
    if (s_running) {
        return ESP_OK;   /* 幂等 */
    }
    if (!s_slot_mutex) {
        s_slot_mutex = xSemaphoreCreateMutex();
    }
    if (!s_slot_mutex) {
        return ESP_ERR_NO_MEM;
    }
    s_running = true;
    if (xTaskCreate(listen_task, "tcp_ctrl_listen", 6144, NULL, 5,
                    &s_listen_task) != pdPASS) {
        s_running = false;
        ESP_LOGE(TAG, "failed to create listen task");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "TCP LED control started");
    return ESP_OK;
}
