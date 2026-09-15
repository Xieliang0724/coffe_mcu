/*
 * ble_led.h - 咖啡台 BLE GATT 控制服务
 *
 * 通过 BLE（NimBLE，BLE 5.0）暴露 GATT 服务，App 连接后写入 JSON 命令控制
 * 10 路 LED，设备通过 Notify 回传应答。命令语义与 TCP(9001) 完全一致（复用 led_cmd）。
 */
#pragma once
#include "esp_err.h"

/* 启动 BLE GATT 控制服务（NimBLE） */
esp_err_t ble_led_init(void);
