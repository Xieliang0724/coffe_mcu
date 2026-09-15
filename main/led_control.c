/*
 * led_control.c - 无界coffee · 10 路 LED 开关控制实现
 *
 * 引脚：见 s_pins[]，默认按 ESP32-C5（N16R8）避开所有冲突脚：
 *   - Strapping : 2,3,7,25,26,27,28
 *   - PSRAM     : 15（N16R8 带 8MB PSRAM 时被占用）
 *   - 项目已用  : 5,6(Modbus),9(复位),27(RGB),11,12(串口),19,20(USB)
 * 故选用的 10 路：0,1,4,8,13,14,16,17,23,24
 * ⚠️ 接板前请对照你的 ESP32-C5-N16R8 板丝印确认这些脚都已引出且可用。
 *    如需改动，直接改 s_pins[] 数组即可。
 *
 * 激活电平：本项目外接的为「NPN/漏极输出」低边开关板；
 * 高电平(1) = 输出拉到 GND = 灯亮。若你的开关板为低电平触发，
 * 请把下面 ACTIVE_LOW 置 1（或按板子说明调整）。
 */
#include "led_control.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "led_ctrl";

/* 10 路 GPIO：CH1..CH10 */
static gpio_num_t s_pins[LED_CHANNEL_COUNT] = {
    GPIO_NUM_0,  // CH1
    GPIO_NUM_1,  // CH2
    GPIO_NUM_4,  // CH3
    GPIO_NUM_8,  // CH4
    GPIO_NUM_13, // CH5
    GPIO_NUM_14, // CH6
    GPIO_NUM_16, // CH7
    GPIO_NUM_17, // CH8
    GPIO_NUM_23, // CH9
    GPIO_NUM_24, // CH10
};

static bool s_states[LED_CHANNEL_COUNT] = {false};

/* NVS：led 命名空间，key = "st"，一个字节位图存 10 路状态 */
static const char *NVS_NS = "coffee_led";
static const char *NVS_KEY = "st";

/* 激活电平：0 = 高电平触发（输出高=灯亮，默认）
 * 若你的开关板是「低电平触发」，把这里改成 1。 */
#define ACTIVE_LOW 0

#if ACTIVE_LOW
#define LEVEL_ON  0
#define LEVEL_OFF 1
#else
#define LEVEL_ON  1
#define LEVEL_OFF 0
#endif

static void apply_channel(uint8_t idx, bool on)
{
    gpio_set_level(s_pins[idx], on ? LEVEL_ON : LEVEL_OFF);
}

void led_control_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 0,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    for (int i = 0; i < LED_CHANNEL_COUNT; i++) {
        io.pin_bit_mask |= 1ULL << s_pins[i];
    }
    gpio_config(&io);

    /* 上电先全部置 OFF，避免复位瞬间误亮 */
    for (int i = 0; i < LED_CHANNEL_COUNT; i++) {
        apply_channel(i, false);
    }

    /* 从 NVS 恢复上次状态 */
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t bitmap = 0;
        if (nvs_get_u8(h, NVS_KEY, &bitmap) == ESP_OK) {
            ESP_LOGI(TAG, "从 NVS 恢复 LED 状态: 0x%02X", bitmap);
            for (int i = 0; i < LED_CHANNEL_COUNT; i++) {
                s_states[i] = (bitmap >> i) & 0x01;
                apply_channel(i, s_states[i]);
            }
        }
        nvs_close(h);
    }
}

bool led_set(uint8_t ch, bool on)
{
    if (ch < 1 || ch > LED_CHANNEL_COUNT) {
        ESP_LOGW(TAG, "非法通道 %u (1..%d)", ch, LED_CHANNEL_COUNT);
        return false;
    }
    uint8_t idx = ch - 1;
    s_states[idx] = on;
    apply_channel(idx, on);

    /* 持久化 */
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        uint8_t bitmap = 0;
        for (int i = 0; i < LED_CHANNEL_COUNT; i++) {
            bitmap |= (uint8_t)(s_states[i] ? (1u << i) : 0u);
        }
        nvs_set_u8(h, NVS_KEY, bitmap);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "CH%u -> %s", ch, on ? "ON" : "OFF");
    return true;
}

bool led_get(uint8_t ch)
{
    if (ch < 1 || ch > LED_CHANNEL_COUNT) {
        return false;
    }
    return s_states[ch - 1];
}

int led_channel_count(void)
{
    return LED_CHANNEL_COUNT;
}
