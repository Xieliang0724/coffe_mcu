/*
 * tcp_ctrl.h - 个性化 TCP LED 控制服务（JSON-over-TCP）
 *
 * 监听固定端口 9001，客户作为 TCP 客户端主动连接，按
 * docs/2026-09-15_LED控制TCP协议_v1.0.md 的 JSON 协议控制 10 路 LED，
 * 执行完毕后回一个应答报文。
 */
#pragma once
#include "esp_err.h"

/* 启动 TCP LED 控制服务（固定端口 9001） */
esp_err_t tcp_ctrl_init(void);
