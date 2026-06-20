#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化 USB 设备 */
void init_usb_host();

/** 初始化 LED、按钮任务、WiFi 状态回调（不含 WiFi/NVS 初始化） */
void board_setup(void);

/** WiFi 初始化完成后同步 LED 状态 */
void board_sync_wifi_led(void);

/**0--
 * @brief 将字节数转换为 MB（兆字节），保留浮点数精度
 * @param bytes 字节数（size_t 类型）
 * @return 对应的 MB 值（float）
 */
static inline float bytes_to_mb(size_t bytes) {
    return bytes / (1024.0f * 1024.0f);
}

#ifdef __cplusplus
}
#endif
