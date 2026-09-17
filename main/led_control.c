/*
 * led_control.c - 无界coffee · 10 路 LED 开关控制实现
 *
 * 引脚：见 s_pins[]，均取自 E101-C5WN8-PS 开发板排针引出的有效 GPIO
 *   - 必须避开 ESP32-C5 上不存在的引脚(如 GPIO16/17/18-22)，否则 CPU_LOCKUP
 *   - PSRAM  : 15（模组带 PSRAM 时被占用）
 *   - 已用   : 9(复位),27(RGB),11,12(串口)
 *   - 说明   : 现用 GPIO6 作为 CH8，原 GPIO5/6 预留的 485(UART1) 暂缓
 * 故选用的 10 路：0,1,4,8,13,14,10,6,23,24
 * ⚠️ 接板前请对照开发板丝印确认这些脚都已引出且可用。
 *    如需改动，直接改 s_pins[] 数组即可。
 *
 * 状态模型：LED 状态**仅保存在内存**，**不写入 NVS/Flash**。
 * 设备重启（含掉电）后所有通道恢复为灭。原因：客户可能高频切换
 * （流水灯/呼吸等效果），每次落盘会反复擦写 Flash 损耗寿命。
 * 如后续需要“断电记忆”，将另行提供带写入节流的保存命令。
 *
 * 激活电平：本项目外接的为「NPN/漏极输出」低边开关板；
 * 高电平(1) = 输出拉到 GND = 灯亮。若你的开关板为低电平触发，
 * 请把下面 ACTIVE_LOW 置 1（或按板子说明调整）。
 */
#include "led_control.h"

#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "led_ctrl";

/* 10 路 GPIO：CH1..CH10 */
/* 10 路 GPIO：CH1..CH10（均取自 E101-C5WN8-PS 开发板排针引出的有效 GPIO）
 * 注意：ESP32-C5 的 GPIO16/17 等引脚不存在/未引出，接入会触发 CPU_LOCKUP，
 * 故此处仅选用 datasheet 管脚表确认存在的引脚。 */
static gpio_num_t s_pins[LED_CHANNEL_COUNT] = {
    GPIO_NUM_0,  // CH1
    GPIO_NUM_1,  // CH2
    GPIO_NUM_4,  // CH3
    GPIO_NUM_8,  // CH4
    GPIO_NUM_13, // CH5
    GPIO_NUM_14, // CH6
    GPIO_NUM_10, // CH7
    GPIO_NUM_6,  // CH8
    GPIO_NUM_23, // CH9
    GPIO_NUM_24, // CH10
};

static bool s_states[LED_CHANNEL_COUNT] = {false};

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

    /* 上电所有通道置 OFF；状态仅内存，不落盘、上电不恢复 */
    for (int i = 0; i < LED_CHANNEL_COUNT; i++) {
        s_states[i] = false;
        apply_channel(i, false);
    }
    ESP_LOGI(TAG, "LED init done (state in-memory only, all OFF)");
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
