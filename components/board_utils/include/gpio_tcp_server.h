#pragma once

#include "led_manager.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** GPIO 控制 TCP 端口（与 USB/IP 3240 分离） */
#define GPIO_TCP_PORT 8080

/**
 * @brief 启动 GPIO TCP 服务（Core 0、低优先级 FreeRTOS 任务）
 * @param led_handle 可为 NULL；非 NULL 时支持 LED 命令
 */
void gpio_tcp_server_init(led_handle_t *led_handle);

#ifdef __cplusplus
}
#endif
