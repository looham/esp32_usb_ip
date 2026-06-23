# ESP32 USB/IP 网关

将 ESP32 变成 USB/IP 服务器，通过网络远程共享物理 USB 设备。

## 功能特性

- **USB/IP 协议服务**：在 TCP 3240 端口提供标准 USB/IP 协议，Linux 主机可直接 `usbip attach`
- **USB 设备热插拔**：自动检测 USB 设备插入/拔出，动态绑定/解绑，无需重启
- **大传输分块**：自动将大块 USB 传输拆分为小 chunk，降低 ESP32 DMA 内存压力
- **WiFi 配网**：支持 STA 连接和 AP Web 配网门户（SSID: `登录-192.168.4.1`）
- **GPIO 远程控制**：独立 TCP 8080 端口，支持远程读写 GPIO 和控制 RGB LED
- **状态指示**：板载 WS2812 LED 通过颜色反映 WiFi 状态和按钮操作
- **双芯片支持**：ESP32-S3（内置 WiFi）和 ESP32-P4（外接 WiFi 芯片）

## 支持的硬件

| 芯片 | Flash | PSRAM | WiFi | USB PHY |
|:---|:---|:---|:---|:---|
| ESP32-S3 | 16MB QIO | 8MB Octal | 内置 | OTG |
| ESP32-P4 | 32MB QIO | 外接 | esp_hosted | DWC |

## 快速开始

### 环境准备

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/) v5.5+
- Python 3.8+

### 编译与烧录

```bash
# 设置 ESP-IDF 环境
. $IDF_PATH/export.sh

# 选择目标芯片（二选一）
idf.py set-target esp32s3
# 或
idf.py set-target esp32p4

# 编译
idf.py build

# 烧录
idf.py -p /dev/ttyUSB0 flash monitor
```

### WiFi 配网

1. 首次启动无 WiFi 配置，ESP32 自动进入 AP 模式
2. 连接 WiFi `登录-192.168.4.1`（无密码）
3. 浏览器打开 `http://192.168.4.1`，选择 WiFi 并输入密码
4. 提交后设备自动重启并连接指定 WiFi

也可通过 menuconfig 预设 WiFi 信息：

```bash
idf.py menuconfig
# → Usbipdcpp WiFi Configuration
```

### Linux 主机连接 USB 设备

```bash
# 安装 usbip 工具
sudo apt install linux-tools-generic hwdata

# 加载内核模块
sudo modprobe vhci-hcd

# 查看远程设备列表（替换为 ESP32 的 IP）
usbip list -r 192.168.x.x

# 绑定远程设备
sudo usbip attach -r 192.168.x.x -b 1-1

# 查看已绑定设备
usbip port

# 解除绑定
sudo usbip detach -p 00
```

### Windows 主机连接 USB 设备
使用 [USB/IP 工具](https://github.com/vadimgrn/usbip-win2) 连接 ESP32 的 USB/IP 服务器。

## 硬件接线

### ESP32-S3 引脚分配

| 引脚 | 功能 | 说明 |
|:---|:---|:---|
| GPIO 48 | RGB LED | WS2812（RMT 驱动，勿直接 GPIO 控制） |
| GPIO 0 | BOOT 按钮 | 内部上拉，按下低电平 |
| GPIO 14 | 外接按钮 1 | 可选 |
| GPIO 39 | 外接按钮 2 | 可选 |
| GPIO 1 | 通用输出 1 | 默认使能脚 / USB 电源开关 |
| GPIO 2 | 通用输出 2 | 空闲安全 IO |
| GPIO 21 | 通用输出 3 | 空闲安全 IO |
| GPIO 47 | 通用输出 4 | 空闲安全 IO |
| GPIO 19/20 | USB D-/D+ | **禁止占用** |

### 按钮操作

| 操作 | 按住时长 | 功能 |
|:---|:---|:---|
| 单击 | < 1 秒 | LED 变蓝色（提示反馈） |
| 长按 | 1~5 秒 | 切换 LED 开关 |
| 超长按 | ≥ 5 秒 | 清除 WiFi 配置，3 秒后重启 |

### LED 状态指示

| 颜色 | 含义 |
|:---|:---|
| 红色 | 启动中 / WiFi 断开 |
| 黄色 | WiFi 连接中 / AP 配网模式 |
| 绿色 | WiFi 已连接 |
| 蓝色 | 单击按钮反馈 |
| 紫色 | 正在清除 WiFi 配置 |

## GPIO TCP 协议

独立端口 8080，行文本协议（`\n` 结尾），单客户端，30 秒超时。

| 命令 | 格式 | 响应 | 说明 |
|:---|:---|:---|:---|
| PING | `PING` | `PONG` | 心跳检测 |
| GET | `GET <pin>` | `OK <pin> <level>` | 读取引脚电平 |
| SET | `SET <pin> <0\|1>` | `OK` | 设置输出电平 |
| LED | `LED <r> <g> <b>` | `OK` | 设置 RGB LED 颜色 |

```bash
# 示例：远程控制
echo -e "PING\n" | nc 192.168.x.x 8080
echo -e "GET 1\n" | nc 192.168.x.x 8080
echo -e "SET 1 1\n" | nc 192.168.x.x 8080
echo -e "LED 0 255 0\n" | nc 192.168.x.x 8080
```

## 感谢
- 感谢项目 [usbipdcpp](https://github.com/yunsmall/usbipdcpp_esp32) 提供的 USB/IP 服务器实现。

## 许可证

- 本项目：[Apache License 2.0](LICENSE)
- usbipdcpp 子模块：[LGPL-3.0](components/usbipdcpp/usbipdcpp/LICENSE)
