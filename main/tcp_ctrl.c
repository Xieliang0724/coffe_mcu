/*
 * tcp_ctrl.c - 个性化 TCP LED 控制服务（JSON-over-TCP）
 *
 * 监听固定端口 9001，按 docs/2026-09-15_LED控制TCP协议_v1.0.1.md 处理：
 *   每条命令 = 单行 JSON + '\n'；执行完回一个 JSON + '\n'。
 * 命令分发复用 led_cmd（与 BLE 共用同一套语义）。
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
#include "led_cmd.h"

static const char *TAG = "tcp_ctrl";

#define CTRL_PORT       9001
#define MAX_CLIENTS     4
#define MAX_LINE        1024         /* 单条命令最大长度（含 '\n'），超长本行丢弃并回错 */
#define CLIENT_IDLE_MS  300000       /* 客户端空闲超时：300s 无数据则关闭，释放槽位 */

static bool s_running = false;
static SemaphoreHandle_t s_slot_mutex = NULL;
static int s_client_fds[MAX_CLIENTS] = {-1, -1, -1, -1};
static TaskHandle_t s_client_tasks[MAX_CLIENTS] = {NULL};
static TaskHandle_t s_listen_task = NULL;
static int s_listen_fd = -1;

/* 发送一行文本 + '\n' */
static void send_line(int fd, const char *s, size_t len)
{
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
}

/* 处理一行：执行并回应答 */
static void handle_line(int fd, const char *line)
{
    char *resp = led_cmd_execute(line);
    if (!resp) {
        return;
    }
    send_line(fd, resp, strlen(resp));
    free(resp);
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
    bool skip_nl = false;   /* 超长行丢弃模式：直到下一个 '\n' */

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
            if (skip_nl) {
                /* 超长行剩余字符直接丢弃，到行尾为止 */
                if (c == '\n') {
                    skip_nl = false;
                }
                continue;
            }
            if (c == '\n') {
                buf[len] = '\0';
                if (len > 0) {
                    handle_line(fd, buf);
                }
                len = 0;
            } else if (len < sizeof(buf) - 1) {
                buf[len++] = c;
            } else {
                /* 单条超长：本行回一个 INVALID_JSON，并丢弃该行剩余字符 */
                cJSON *resp = cJSON_CreateObject();
                cJSON_AddBoolToObject(resp, "ok", false);
                cJSON_AddStringToObject(resp, "err", "INVALID_JSON");
                cJSON_AddStringToObject(resp, "msg", "命令过长");
                char *s = cJSON_PrintUnformatted(resp);
                if (s) {
                    send_line(fd, s, strlen(s));
                    cJSON_free(s);
                }
                cJSON_Delete(resp);
                len = 0;
                skip_nl = true;
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
        /* 客户端空闲超时：300s 无数据自动关闭，释放槽位，防死连接占满 */
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
