/**
 * @file board.h
 * @brief 板级管理接口：USB Host 初始化、板级初始化、WiFi LED 同步、字节转换工具
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 USB Host 驱动并启动事件处理线程
 *
 * 安装 USB Host 库，创建事件处理线程（Core 1, prio 10, 4KB 栈）。
 * 线程负责处理 USB 设备热插拔等底层事件，所有客户端注销且设备释放后自动卸载。
 */
void init_usb_host();

/**
 * @brief 初始化板级外设（不含 WiFi/NVS/USB Host）
 *
 * 初始化内容：
 * 1. WS2812 RGB LED（GPIO 48，亮度 80，启动红色）
 * 2. WiFi 状态回调注册（LED 自动反映 WiFi 状态）
 * 3. BOOT 按钮扫描任务（单击蓝/长按开关/5秒清WiFi）
 * 4. GPIO TCP 服务器（端口 8080）
 */
void board_setup(void);

/**
 * @brief WiFi 初始化完成后同步 LED 状态
 *
 * 根据 WiFi 实际状态设置 LED 颜色：
 * - STA 已连接 → 绿色
 * - AP 配网模式 → 黄色
 * - 未连接 → 红色
 */
void board_sync_wifi_led(void);

/**
 * @brief 将字节数转换为 MB（兆字节），保留浮点数精度
 * @param bytes 字节数（size_t 类型）
 * @return 对应的 MB 值（float）
 *
 * 用于内存监控日志输出，例：
 *   ESP_LOGI(TAG, "Free: %.2f MB", bytes_to_mb(esp_get_free_heap_size()));
 */
static inline float bytes_to_mb(size_t bytes) {
    return bytes / (1024.0f * 1024.0f);
}

#ifdef __cplusplus
}
#endif
