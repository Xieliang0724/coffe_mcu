/*
 * led_cmd.h - 咖啡台 LED 命令分发（JSON 解析 + 执行 + 组应答）
 *
 * TCP(9001) 与 BLE GATT 共用同一套命令语义：
 *   输入一行 JSON 命令，返回 malloc 的应答 JSON 字符串（调用者负责 free）。
 */
#pragma once

/* 解析并执行一条 JSON LED 命令。
 * 返回 malloc 的应答 JSON 字符串（含 ok/err/data），永远非 NULL；调用者 free。
 * 输入非法 JSON / 空串等也会得到带 ok:false 的应答。 */
char *led_cmd_execute(const char *json);
