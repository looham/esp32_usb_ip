/**
 * @file pin_define.h
 * @brief ESP32-S3 引脚定义（以 DevKitC-1 + Octal PSRAM + USB OTG 为参考）
 *
 * 接线约定：
 * - 按钮：内部上拉，按下接 GND（低电平有效）
 * - GPIO 输出：3.3V 逻辑，勿直接驱动大电流负载（请加三极管/MOS/继电器模块）
 *
 * ┌─────────────────────────────────────────────────────────────┐
 * │ 禁止占用（本 USB/IP 工程已使用或硬件固定）                    │
 * ├─────────────────────────────────────────────────────────────┤
 * │ GPIO 19/20  USB OTG D-/D+                                   │
 * │ GPIO 26-32  Octal Flash / PSRAM                            │
 * │ GPIO 43/44  USB 串口 / 默认 UART 控制台                     │
 * │ GPIO 45/46  Strapping（启动模式 / VDD_SPI 等）               │
 * │ GPIO 48     板载 WS2812（RMT，非普通 GPIO 输出）              │
 * └─────────────────────────────────────────────────────────────┘
 *
 * 慎用（Strapping，仅建议作 BOOT 键，勿外接强下拉）：
 *   GPIO 0（BOOT）、GPIO 3（JTAG）
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========== 板载固定功能 ========== */

/** 板载 RGB LED（WS2812，经 RMT 驱动，勿用 SET 命令直接控此脚） */
#define LED_PIN 48

/** 兼容旧名：通用使能输出 = GPIO_OUT_1 */
#define ENABLE_PIN GPIO_OUT_1

/* ========== 按钮引脚（输入，内部上拉，低电平=按下） ========== */

/** 板载 BOOT 键（Strapping：上电时勿长按保持低电平） */
#define BOOT_BUTTON_GPIO 0

/** 外接按钮 1（推荐：Header 可及、无特殊功能） */
#define BUTTON1_GPIO 14

/** 外接按钮 2（S3 上为普通 IO；若未焊接可改为 21/38/47） */
#define BUTTON2_GPIO 39

/** 可选外接按钮 3/4（默认未在 button_manager 启用，供扩展） */
#define BUTTON3_GPIO 21
#define BUTTON4_GPIO 47

/** 已在工程中启用的按钮列表（board.cpp / button_manager） */
#define BOARD_BUTTON_PIN_LIST BOOT_BUTTON_GPIO, BUTTON1_GPIO, BUTTON2_GPIO
#define BOARD_BUTTON_PIN_COUNT 3

/* ========== 通用 GPIO 输出（可 TCP SET / 继电器 / LED 使能等） ========== */

#define GPIO_OUT_1 1  /**< 默认使能脚（如 USB 设备电源开关、继电器） */
#define GPIO_OUT_2 2  /**< 空闲安全 IO */
#define GPIO_OUT_3 21 /**< 空闲安全 IO */
#define GPIO_OUT_4 47 /**< 空闲安全 IO */

/** 允许远程 SET 的输出引脚白名单 */
#define BOARD_GPIO_OUTPUT_PIN_LIST GPIO_OUT_1, GPIO_OUT_2, GPIO_OUT_3, GPIO_OUT_4
#define BOARD_GPIO_OUTPUT_PIN_COUNT 4

/** 允许远程 GET 的引脚 = 输出白名单 + 按钮列表 */
#define BOARD_GPIO_INPUT_PIN_LIST BOARD_GPIO_OUTPUT_PIN_LIST, BOARD_BUTTON_PIN_LIST
#define BOARD_GPIO_INPUT_PIN_COUNT (BOARD_GPIO_OUTPUT_PIN_COUNT + BOARD_BUTTON_PIN_COUNT)

/* ========== 可选：DVP 摄像头（与下方 GPIO 冲突，默认不启用） ========== */

#ifndef BOARD_HAS_CAMERA
#define BOARD_HAS_CAMERA 0
#endif

#if BOARD_HAS_CAMERA
#define CAM_PWDN_GPIO_NUM -1
#define CAM_RESET_GPIO_NUM -1
#define CAM_XCLK_GPIO_NUM 8
#define CAM_SIOD_GPIO_NUM 13
#define CAM_SIOC_GPIO_NUM 12
#define CAM_Y2_GPIO_NUM 7
#define CAM_Y3_GPIO_NUM 5
#define CAM_Y4_GPIO_NUM 4
#define CAM_Y5_GPIO_NUM 6
#define CAM_Y6_GPIO_NUM 15
#define CAM_Y7_GPIO_NUM 17
#define CAM_Y8_GPIO_NUM 18
#define CAM_Y9_GPIO_NUM 9
#define CAM_VSYNC_GPIO_NUM 11
#define CAM_HREF_GPIO_NUM 10
#define CAM_PCLK_GPIO_NUM 16
#endif

/* ========== 辅助查询（gpio_tcp_server 等使用） ========== */

/** 引脚是否在 int 数组中 */
static inline bool pin_in_list(int pin, const int *list, size_t count)
{
    if (pin < 0)
        return false;
    for (size_t i = 0; i < count; i++)
    {
        if (list[i] == pin)
            return true;
    }
    return false;
}

/** 是否允许作为 GPIO 输出（SET） */
static inline bool pin_board_output_allowed(int pin)
{
    static const int outs[] = {BOARD_GPIO_OUTPUT_PIN_LIST};
    return pin_in_list(pin, outs, BOARD_GPIO_OUTPUT_PIN_COUNT);
}

/** 是否允许读取电平（GET） */
static inline bool pin_board_input_allowed(int pin)
{
    static const int ins[] = {BOARD_GPIO_INPUT_PIN_LIST};
    return pin_in_list(pin, ins, BOARD_GPIO_INPUT_PIN_COUNT);
}

/** 是否为按钮引脚（GET 时自动配置上拉输入） */
static inline bool pin_board_is_button(int pin)
{
    static const int btns[] = {BOARD_BUTTON_PIN_LIST};
    return pin_in_list(pin, btns, BOARD_BUTTON_PIN_COUNT);
}

#ifdef __cplusplus
}
#endif
