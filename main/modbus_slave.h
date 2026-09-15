/*
 * modbus_slave.h - Modbus TCP 从站（服务器）接口
 *
 * coffe_mcu 自身作为一个 Modbus TCP 从站，监听固定端口 502，
 * 通过**保持寄存器**暴露咖啡台 10 路 LED：
 *   寄存器地址 0x0000..0x0009 = 灯片 CH1..CH10，值 0/1 = 灭/亮。
 *
 * 支持功能码：0x03 读保持寄存器 / 0x06 写单个寄存器 / 0x10 写多个寄存器。
 * 纯网络实现，不占用任何串口（UART1 保留给 485 舵机）。
 */
#pragma once
#include "esp_err.h"

/* 启动 Modbus TCP 从站（固定端口 502，上电即开） */
esp_err_t modbus_slave_init(void);
