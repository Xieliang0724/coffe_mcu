/*
 * led_control.h - 无界coffee · 10 路 LED 开关控制
 *
 * 咖啡台 10 片 12V 灯片，由 ESP32 GPIO 驱动外部的 NPN/漏极输出开关板
 * （低边开关）逐路控制亮/灭。本模块负责：
 *   - GPIO 初始化（10 路输出）
 *   - 开关状态管理 + NVS 持久化（断电重启保持上次亮灭）
 *   - 供 Web 接口调用（见 web_server 的 /api/leds、/api/led）
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LED_CHANNEL_COUNT 10

/* 初始化 10 路 GPIO 输出，并从 NVS 恢复上次开关状态 */
void led_control_init(void);

/* 设置某路开关：ch 取 1..LED_CHANNEL_COUNT，on=true 亮、false 灭。成功返回 true */
bool led_set(uint8_t ch, bool on);

/* 查询某路当前状态：ch 取 1..LED_CHANNEL_COUNT */
bool led_get(uint8_t ch);

/* 通道总数 */
int led_channel_count(void);

#ifdef __cplusplus
}
#endif
