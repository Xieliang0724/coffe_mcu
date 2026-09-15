/*
 * modbus_slave.c - Modbus TCP 从站（服务器）实现
 *
 * 监听固定端口 502，把 10 路 LED 以保持寄存器形式暴露给上位机：
 *   - 寄存器地址 addr = 0x0000..0x0009  <->  灯片 CH = addr+1
 *   - 值 0/1  <->  灭/亮
 *
 * 功能码：
 *   0x03  读保持寄存器
 *   0x06  写单个寄存器
 *   0x10  写多个寄存器
 *
 * 纯网络实现（lwIP socket），不占用任何串口/UART。
 */
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"

#include "modbus_slave.h"
#include "led_control.h"

static const char *TAG = "mb_tcp";

#define MB_PORT         502            /* Modbus TCP 标准端口，固定 */

#define MAX_CLIENTS     4              /* 并发客户端上限 */

#define REG_LED_BASE    0              /* 寄存器起始地址 */
#define REG_LED_COUNT   10             /* 10 路 LED，各占一个寄存器 */

#define MBAP_LEN        7              /* 事务ID(2)+协议ID(2)+长度(2)+单元ID(1) */
#define MAX_REQ_LEN     260

/* 功能码 */
#define FC_READ_HOLD    0x03
#define FC_WRITE_SINGLE 0x06
#define FC_WRITE_MULTI  0x10

/* 异常码 */
#define EXC_ILLEGAL_FUNC  0x01
#define EXC_ILLEGAL_ADDR  0x02
#define EXC_ILLEGAL_VALUE 0x03

static bool s_running = false;
static SemaphoreHandle_t s_slot_mutex = NULL;
static int s_client_fds[MAX_CLIENTS] = {-1, -1, -1, -1};
static TaskHandle_t s_client_tasks[MAX_CLIENTS] = {NULL};
static TaskHandle_t s_listen_task = NULL;
static int s_listen_fd = -1;

/* 寄存器访问：把 Modbus 寄存器地址映射到 LED 通道 */
static bool reg_valid(uint16_t addr)
{
    return addr < REG_LED_COUNT;
}

static uint16_t reg_get(uint16_t addr)
{
    /* addr+1 = 灯片通道号 */
    return led_get((uint8_t)(addr + 1)) ? 1 : 0;
}

static bool reg_set(uint16_t addr, uint16_t val)
{
    return led_set((uint8_t)(addr + 1), val != 0);
}

/* ------------------------------------------------------------------ */
/* 功能码处理：成功返回 0 并把响应 PDU 写入 resp；失败返回异常码        */
/* ------------------------------------------------------------------ */

/* 0x03 读保持寄存器 */
static uint8_t read_holding(const uint8_t *pdu, size_t plen,
                            uint8_t *resp, size_t *rlen)
{
    if (plen < 5) {
        return EXC_ILLEGAL_VALUE;
    }
    uint16_t addr = (pdu[1] << 8) | pdu[2];
    uint16_t qty  = (pdu[3] << 8) | pdu[4];
    if (qty < 1 || qty > 125) {
        return EXC_ILLEGAL_VALUE;
    }
    if ((uint32_t)addr + qty > REG_LED_COUNT) {
        return EXC_ILLEGAL_ADDR;
    }
    resp[0] = FC_READ_HOLD;
    resp[1] = (uint8_t)(qty * 2);
    for (uint16_t i = 0; i < qty; i++) {
        uint16_t v = reg_get(addr + i);
        resp[2 + 2 * i]      = (uint8_t)(v >> 8);
        resp[3 + 2 * i]      = (uint8_t)(v & 0xFF);
    }
    *rlen = 2 + qty * 2;
    return 0;
}

/* 0x06 写单个寄存器 */
static uint8_t write_single(const uint8_t *pdu, size_t plen,
                            uint8_t *resp, size_t *rlen)
{
    if (plen < 5) {
        return EXC_ILLEGAL_VALUE;
    }
    uint16_t addr = (pdu[1] << 8) | pdu[2];
    uint16_t val  = (pdu[3] << 8) | pdu[4];
    if (!reg_valid(addr)) {
        return EXC_ILLEGAL_ADDR;
    }
    if (val > 1) {
        return EXC_ILLEGAL_VALUE;
    }
    reg_set(addr, val);
    memcpy(resp, pdu, 5);   /* 回显请求 */
    *rlen = 5;
    return 0;
}

/* 0x10 写多个寄存器 */
static uint8_t write_multi(const uint8_t *pdu, size_t plen,
                           uint8_t *resp, size_t *rlen)
{
    if (plen < 6) {
        return EXC_ILLEGAL_VALUE;
    }
    uint16_t addr = (pdu[1] << 8) | pdu[2];
    uint16_t qty  = (pdu[3] << 8) | pdu[4];
    uint8_t  bc   = pdu[5];
    if (qty < 1 || qty > 123) {
        return EXC_ILLEGAL_VALUE;
    }
    if ((uint32_t)addr + qty > REG_LED_COUNT) {
        return EXC_ILLEGAL_ADDR;
    }
    if (bc != qty * 2 || (size_t)(6 + bc) > plen) {
        return EXC_ILLEGAL_VALUE;
    }
    /* 先整体校验值合法，再写入，避免部分成功 */
    for (uint16_t i = 0; i < qty; i++) {
        uint16_t v = (pdu[6 + 2 * i] << 8) | pdu[7 + 2 * i];
        if (v > 1) {
            return EXC_ILLEGAL_VALUE;
        }
    }
    for (uint16_t i = 0; i < qty; i++) {
        uint16_t v = (pdu[6 + 2 * i] << 8) | pdu[7 + 2 * i];
        reg_set(addr + i, v);
    }
    resp[0] = FC_WRITE_MULTI;
    resp[1] = (uint8_t)(addr >> 8);
    resp[2] = (uint8_t)(addr & 0xFF);
    resp[3] = (uint8_t)(qty >> 8);
    resp[4] = (uint8_t)(qty & 0xFF);
    *rlen = 5;
    return 0;
}

/* ------------------------------------------------------------------ */
/* 客户端连接处理                                                       */
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

    uint8_t mbap[MBAP_LEN];
    uint8_t pdu[MAX_REQ_LEN];

    while (s_running) {
        /* 读 MBAP 头 */
        size_t got = 0;
        while (got < MBAP_LEN && s_running) {
            int n = recv(fd, mbap + got, MBAP_LEN - got, 0);
            if (n <= 0) goto done;
            got += n;
        }
        uint16_t tid = (mbap[0] << 8) | mbap[1];
        uint16_t pid = (mbap[2] << 8) | mbap[3];
        uint16_t len = (mbap[4] << 8) | mbap[5];
        uint8_t  uid = mbap[6];
        if (pid != 0 || len < 2 || (len - 1) > sizeof(pdu)) {
            goto done;
        }
        /* 读 PDU */
        size_t plen = len - 1;
        got = 0;
        while (got < plen && s_running) {
            int n = recv(fd, pdu + got, plen - got, 0);
            if (n <= 0) goto done;
            got += n;
        }

        uint8_t fc = pdu[0];
        uint8_t resp_pdu[264];
        size_t rlen = 0;
        uint8_t exc = 0;

        switch (fc) {
        case FC_READ_HOLD:    exc = read_holding(pdu, plen, resp_pdu, &rlen);    break;
        case FC_WRITE_SINGLE: exc = write_single(pdu, plen, resp_pdu, &rlen);    break;
        case FC_WRITE_MULTI:  exc = write_multi(pdu, plen, resp_pdu, &rlen);     break;
        default:              exc = EXC_ILLEGAL_FUNC; break;
        }

        /* 组响应 PDU：正常 = fc+data；异常 = (fc|0x80)+exc */
        uint8_t rpdu[264];
        size_t rpdu_len;
        if (exc) {
            rpdu[0] = (uint8_t)(fc | 0x80);
            rpdu[1] = exc;
            rpdu_len = 2;
        } else {
            memcpy(rpdu, resp_pdu, rlen);
            rpdu_len = rlen;
        }

        /* 组 MBAP 响应：[tid][0x0000][len'=1+rpdu_len][uid][PDU] */
        uint8_t rsp[MBAP_LEN + 264];
        rsp[0] = (uint8_t)(tid >> 8);
        rsp[1] = (uint8_t)(tid & 0xFF);
        rsp[2] = 0;
        rsp[3] = 0;
        uint16_t rlen16 = 1 + rpdu_len;
        rsp[4] = (uint8_t)(rlen16 >> 8);
        rsp[5] = (uint8_t)(rlen16 & 0xFF);
        rsp[6] = uid;
        memcpy(rsp + MBAP_LEN, rpdu, rpdu_len);
        size_t total = MBAP_LEN + rpdu_len;
        size_t sent = 0;
        while (sent < total && s_running) {
            int n = send(fd, rsp + sent, total - sent, 0);
            if (n <= 0) goto done;
            sent += n;
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
    addr.sin_port = htons(MB_PORT);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind %u failed", MB_PORT);
        close(fd);
        vTaskDelete(NULL);
        return;
    }
    listen(fd, 4);
    s_listen_fd = fd;
    ESP_LOGI(TAG, "Modbus TCP slave listening on port %u (regs 0x0000..0x%04X = CH1..CH%d)",
             MB_PORT, REG_LED_COUNT - 1, REG_LED_COUNT);

    while (s_running) {
        int client = accept(fd, NULL, NULL);
        if (client < 0) {
            if (!s_running) break;
            continue;
        }
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
        if (xTaskCreate(client_task, "mb_tcp_client", 6144, ca, 6,
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

esp_err_t modbus_slave_init(void)
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
    if (xTaskCreate(listen_task, "mb_tcp_listen", 6144, NULL, 5,
                    &s_listen_task) != pdPASS) {
        s_running = false;
        ESP_LOGE(TAG, "failed to create listen task");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Modbus TCP slave started");
    return ESP_OK;
}
