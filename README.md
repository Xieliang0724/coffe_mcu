# coffe_mcu — 咖啡台智能终端固件

基于 `esp32c5_web_provision` 二次开发的**咖啡台智能终端**固件，面向客户提供多接口控制 10 路 LED；已**移除 TLS**（本项目不需要），并升级为 **支持 OTA** 的分区布局。目标芯片 **ESP32-C5（N16R8：16MB flash / 8MB PSRAM）**。

> 对外客户协议文档（去 MCU 细节）见 `docs/`；本 README 为内部/集成商使用，保留 MCU 细节。

## 功能特性

- 🌐 **Web 配网**：SoftAP 热点 + 网页配网（`http://192.168.4.1`）；支持 SSID 下拉、手动输入、双频扫描
- 🔎 **mDNS**：联网后经 `http://esp32c5.local` 访问
- 📶 **静态/动态 IP** 可选；**SoftAP 可关**；**连接失败自动回退配网**；**失联兜底**（AP/STA 至少一个可用）
- 💾 **Wi-Fi 配置持久化**（NVS），断电重启自动连接；**复位键**（GPIO9 长按 3 秒清配置）
- 💡 **咖啡台 10 路 LED 独立开关**（GPIO，状态仅内存、掉电全灭）
- 🔌 **Modbus TCP 从站**（端口 502）：LED 以保持寄存器暴露给上位机（PLC/HMI/SCADA）
- 🔗 **自定义 TCP 控制协议**（端口 9001）：客户主动连接发 JSON 指令，执行后回应答
- 📶 **蓝牙 BLE 控制**：广播名 `CoffeeTable-LED`，App 经 BLE GATT 控制（NimBLE）
- 🚀 **OTA 就绪**：分区表含 `ota_0`/`ota_1` 两个 app 分区，可做空中升级

> 三套控制接口（TCP 9001 / Modbus 502 / BLE）命令语义一致、可并存，**均不占用串口**；UART1（GPIO5/6）预留给之后的 485 舵机。

### 🛡️ 失联兜底（AP 与 STA 不允许同时死掉）

C5 为**单射频**芯片，固件保证**任何时刻 AP 和 STA 至少一个可用**。STA 断开后**不立即**开 AP（避免频繁跳变），而是连续断开超过 N 秒（默认 15 秒，可配，1~3600）才自动开启热点；重试耗尽后自动回退配网模式。

> ⚠️ 已知：表单"0 = 立即开启"当前按默认 15 秒处理；路由器"静默死亡"时驱动需等 beacon 超时（约 10~60s）才触发兜底。属协议固有延迟。

## 环境要求

- ESP32-C5 开发板（**N16R8：16MB flash / 8MB PSRAM**）
- **ESP-IDF v6.0.1**（本工程按 v6.0.1 API 编译验证）
- VS Code + ESP-IDF 扩展（推荐）

## 目录结构

```
coffe_mcu/
├── CMakeLists.txt
├── CHANGELOG.md
├── partitions.csv            # 分区表（ota_0/ota_1 两个 app 分区）
├── sdkconfig.defaults        # 目标/闪存/BT/自定义分区表默认配置
├── main/
│   ├── app_main.c            # 入口：初始化 WiFi/Web/LED/TCP/BLE/Modbus
│   ├── config_store.[ch]     # NVS 配网配置
│   ├── wifi_mgr.[ch]         # Wi-Fi 状态机（AP/STA/扫描/静态IP/回退/兜底）
│   ├── web_server.[ch]       # HTTP 配网 + REST（/api/...）
│   ├── led_control.[ch]      # 10 路 LED（GPIO，状态仅内存）
│   ├── led_cmd.[ch]          # LED JSON 命令分发（TCP/BLE 共用）
│   ├── tcp_ctrl.[ch]         # 自定义 TCP 控制协议（9001）
│   ├── ble_led.[ch]          # BLE GATT 控制（NimBLE）
│   ├── modbus_slave.[ch]     # Modbus TCP 从站（502）
│   ├── rgb_led.[ch]          # WS2812 状态灯
│   └── www/index.html        # 内嵌网页
└── docs/                     # 对外协议/接口文档（含 PDF）
```

## 编译与烧录

**命令行**（本机需注意中英文路径，构建用 ASCII 路径镜像，见下）：
```bash
# 本机构建提示：coffe_mcu 所在路径含中文，请先 copy 到 ASCII 路径再 build
idf.py set-target esp32c5
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

> 🔧 **本机（含中文工作区路径）构建**：把工程 copy 到 `/tmp/coffe-mcu-build`（`main/certs`、`build`、`sdkconfig` 排除），在该目录 `idf.py build`（IDF 工具链对非 ASCII 路径会失败：`cannot read spec file '...picolibc.specs'`）。

烧录时使用项目自带分区表（`partitions.csv`），会自动写入 `ota_0` 并设置 otadata。

## 使用流程

1. 上电 → 日志出现 `SoftAP ssid=ESP32C5-XXXXXXXX`
2. 手机连热点 `ESP32C5-XXXXXXXX` → 访问 `http://192.168.4.1` 配网
3. 保存并连接后，网页 / TCP(9001) / Modbus(502) / BLE 均可控制 10 路 LED

## 控制接口速览

| 接口 | 地址/端口 | 说明 |
|---|---|---|
| Web 配网 + REST | `http://<设备IP>:80` | `/api/status`、`/api/leds`、`/api/led`、配网接口 |
| 自定义 TCP | `:9001` | JSON 命令：`ping/led_set/led_set_all/led_set_batch/led_status` |
| Modbus TCP 从站 | `:502` | 保持寄存器 `0x0000..0x0009` = CH1..CH10（`0`=灭/`1`=亮），FC `0x03/0x06/0x10` |
| 蓝牙 BLE | 广播 `CoffeeTable-LED` | GATT 服务 `0xFFE0`，CMD(FFE1 写)/RESP(FFE2 通知)/STA(FFE3 读) |

> 客户可用的**协议文档**：
> - TCP：`docs/2026-09-15_LED控制TCP协议_v1.0.1.md` / `.pdf`
> - BLE：`docs/2026-09-15_LED_BLE控制接口_v1.0.md` / `.pdf`

## 咖啡台 LED 控制（内部细节）

- **通道 → GPIO**（`led_control.c`）：CH1..CH10 = `GPIO 0,1,4,8,13,14,16,17,23,24`
- **状态仅内存、掉电全灭**：不写 NVS（避免高频切换损耗 Flash）
- **高有效**：`ACTIVE_LOW=0`（GPIO 置 1 = LED 亮）
- **开关板**：NPN/漏极（低边）输出，输入 3.3V，高电平拉低 12V LED 负载地
- **供电**：ESP32 用 3.3V（板载 5V USB 亦可）；12V LED 单独供电，两者**只共地**，勿让 12V 碰 ESP32

> ⚠️ 接板前请对照 N16R8 丝印确认 GPIO 可用，避开 `GPIO27`(RGB)、`GPIO9`(复位)、`GPIO11/12`(串口)、`GPIO15`(PSRAM)、`GPIO19/20`(USB)、Strapping(`2,3,7,25,26,27,28`)。`GPIO5/6` 预留给 485 舵机。

## 分区表（OTA）

自定义 `partitions.csv`：**`ota_0` / `ota_1` 各 7.5MB**，+`otadata`/`nvs`/`phy_init`。16MB flash，app 分区充足（当前固件 ≈1.4MB，余量 ~81%）。

- **已实现网页 OTA**：网页「🛠 固件升级」上传 `.bin` → `POST /api/ota` 写入另一分区 → 校验 → 切换启动 → 重启；**失败自动回滚**到旧分区（`esp_ota_abort`，启动分区不变）。
- 也可命令行 OTA：`idf.py -p <PORT> ota`（生产可用 `esp_ota` 流程或接入 HTTPS/签名以增强安全）。

## 版本

固件版本号由 `project(coffe_mcu VERSION ...)` 定义，`/api/status` 的 `version` 与开机日志 `App version:` 同步。发布记录见 `CHANGELOG.md`。常用 git 回退/打 tag：

```bash
git log --oneline                # 查看可回滚提交
git reset --hard <commit>        # 本地回退
git push origin main --force     # 需同步远程时（会重写历史，慎重）
```

## 已知限制（内部）

- **所有接口无鉴权/令牌**：HTTP、TCP 9001、Modbus 502 均可被同网段设备读写；BLE 无配对免。请部署于可信网段/VLAN，或量产前启用 v1.1 令牌（协议文档第 7 节）。
- 配网/状态接口走 HTTP 明文；无 TLS。
- AP 兜底时序、重试计数等细节见 `CHANGELOG.md` 历史说明（继承自基座项目的可靠性修复）。
