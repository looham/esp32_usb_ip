/**
 * @file board.cpp
 * @brief ESP32 板级管理：LED 状态指示、按钮事件处理、USB Host 初始化
 *
 * 功能概览：
 * - LED 管理：WS2812 RGB LED 通过颜色反映 WiFi 状态和用户操作
 * - 按钮管理：BOOT 按钮支持单击/长按/超长按三种操作
 * - USB Host：初始化 USB Host 驱动并运行事件处理循环
 *
 * LED 颜色约定：
 * ┌──────────┬────────┬─────────────────────┐
 * │ 颜色     │ RGB值  │ 含义                │
 * ├──────────┼────────┼─────────────────────┤
 * │ 红色     │ 255,0,0│ 启动中 / WiFi 断开   │
 * │ 黄色     │ 255,255,0│ WiFi 连接中 / AP模式│
 * │ 绿色     │ 0,255,0│ WiFi 已连接          │
 * │ 蓝色     │ 0,0,255│ 单击按钮（提示反馈）  │
 * │ 紫色     │ 128,0,128│ 清除 WiFi 配置      │
 * │ 熄灭     │ -      │ 长按关闭 LED         │
 * └──────────┴────────┴─────────────────────┘
 *
 * 按钮操作：
 * - 单击（<1秒）：LED 变蓝色（提示反馈）
 * - 长按（≥1秒）：切换 LED 开关
 * - 超长按（≥5秒）：清除 WiFi 配置，紫色闪烁，3秒后重启
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <esp_log.h>
#include <esp_system.h>
#include <esp_pthread.h>
#include "board.h"
#include "wifi_manager.h"
#include "pin_define.h"
#include "led_manager.h"
#include "gpio_tcp_server.h"
#include <usb/usb_host.h>
#include <thread>
#include "esp32_handler/Esp32Server.h"

#include <driver/gpio.h>
#include <esp_log.h>

static const char *TAG = "BOARD";

/** LED 句柄，由 board_setup() 初始化 */
static led_handle_t *g_led_handle = nullptr;

/** 长按关闭 LED 标志，true 时 LED 熄灭 */
static volatile bool g_led_off = false;

/** 当前 WiFi 状态：0=断开(红), 1=连接中(黄), 2=已连接(绿) */
static volatile int g_wifi_status = 0;

/** USB Host 库事件处理线程（Core 1, prio 10） */
std::thread usb_host_event_thread;

/* 前向声明 */
static void refresh_led(void);
static void wifi_status_callback(int status);
static void button_task(void *arg);

/**
 * @brief 板级初始化（LED + 按钮 + GPIO TCP 服务）
 *
 * 初始化内容：
 * 1. WS2812 RGB LED（GPIO 48）→ 启动时显示红色
 * 2. WiFi 状态回调注册 → LED 自动反映 WiFi 状态
 * 3. 按钮扫描任务（FreeRTOS task, 4KB 栈, prio 5）
 * 4. GPIO TCP 服务器（端口 8080，Core 0）
 *
 * 注意：此函数不含 WiFi/NVS/USB Host 初始化，由调用方按顺序分别调用
 */
void board_setup(void)
{
    // 初始化LED（WS2812 RGB LED，GPIO 48，RMT 驱动）
    led_config_t led_config = {
        .gpio_num = LED_PIN,     // GPIO 48
        .led_num = 1,            // 单颗 LED
        .brightness = 10,        // 亮度 80/255，避免刺眼
    };
    g_led_handle = led_init(&led_config);
    if (g_led_handle)
    {
        g_wifi_status = 0;
        led_set_all(g_led_handle, 255, 0, 0); // 启动时显示红色
    }
    else
    {
        ESP_LOGE(TAG, "LED初始化失败");
    }

    wifi_set_status_callback(wifi_status_callback);                    // 注册 WiFi 状态回调，自动更新 LED
    xTaskCreate(button_task, "button_task", 4096, NULL, 5, NULL);      // 创建按钮扫描任务
    gpio_tcp_server_init(g_led_handle);                                 // 启动 GPIO TCP 服务（端口 8080）
}


/**
 * @brief 初始化 USB Host 驱动并启动事件处理线程
 *
 * USB Host 是 ESP32 作为 USB 主机控制器的软件层，负责：
 * - 枚举插入的 USB 设备
 * - 分配设备地址
 * - 处理设备热插拔事件
 *
 * 事件处理线程运行在 Core 1（与 USB/IP Server 同核），优先级 10（高于主线程的 5），
 * 确保 USB 传输事件得到及时处理。
 *
 * 事件循环退出条件：
 * - 所有客户端注销且所有设备释放后，自动卸载 USB Host 库
 */
void init_usb_host() {
    ESP_LOGI(TAG, "Installing USB Host Library");

    // USB Host 驱动配置
    usb_host_config_t host_config = {
            .skip_phy_setup = false,             // 不跳过 PHY 初始化（需要 USB OTG 硬件就绪）
            .intr_flags = ESP_INTR_FLAG_LEVEL3,  // 中断优先级 3（较高，保证 USB 响应实时性）
            .enum_filter_cb = nullptr,           // 不过滤枚举设备（接受所有 USB 设备）
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));

    // 配置 USB Host 事件处理线程参数
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.prio = 10;                          // 高优先级，确保 USB 事件及时处理
    cfg.pin_to_core = 1;                    // 绑定 Core 1（与 USB/IP Server 同核）
    cfg.thread_name = "usb_host_event_thread";
    cfg.stack_size = 4096;
    esp_pthread_set_cfg(&cfg);

    // 启动 USB Host 库事件处理循环
    usb_host_event_thread = std::thread([]() {
        bool has_clients = true;    // 是否还有客户端注册
        bool has_devices = false;   // 是否还有设备等待释放

        while (has_clients) {
            uint32_t event_flags;
            // 阻塞等待 USB Host 库事件（设备插入/拔出/客户端注销等）
            ESP_ERROR_CHECK(usb_host_lib_handle_events(portMAX_DELAY, &event_flags));

            // 没有客户端注册了，尝试释放所有设备
            if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
                ESP_LOGI(TAG, "Get FLAGS_NO_CLIENTS");
                if (ESP_OK == usb_host_device_free_all()) {
                    ESP_LOGI(TAG, "All devices marked as free, no need to wait FLAGS_ALL_FREE event");
                    has_clients = false;
                }
                else {
                    // 还有设备在使用中，等待所有设备释放
                    ESP_LOGI(TAG, "Wait for the FLAGS_ALL_FREE");
                    has_devices = true;
                }
            }
            // 所有设备都已释放，可以退出循环
            if (has_devices && event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
                ESP_LOGI(TAG, "Get FLAGS_ALL_FREE");
                has_clients = false;
            }
        }
        ESP_LOGI(TAG, "No more clients and devices, uninstall USB Host library");
        ESP_ERROR_CHECK(usb_host_uninstall());
    });

    // 恢复默认 pthread 配置，避免影响后续创建的线程
    auto default_cfg = esp_pthread_get_default_config();
    esp_pthread_set_cfg(&default_cfg);
}


/**
 * @brief 根据 g_led_off 和 g_wifi_status 刷新 LED 颜色
 *
 * LED 优先级：
 * 1. g_led_off = true → 熄灭（长按关闭）
 * 2. g_wifi_status = 0 → 红色（WiFi 断开）
 * 3. g_wifi_status = 1 → 黄色（WiFi 连接中 / AP 模式）
 * 4. g_wifi_status = 2 → 绿色（WiFi 已连接）
 *
 * 注意：单击按钮时 LED 变蓝色不经过此函数，直接在 button_task 中设置
 */
static void refresh_led(void)
{
    if (!g_led_handle)
        return;

    if (g_led_off)
    {
        led_clear(g_led_handle);
        return;
    }

    switch (g_wifi_status)
    {
    case 0:
        led_set_all(g_led_handle, 255, 0, 0);   // 红色：WiFi 未连接
        break;
    case 1:
        led_set_all(g_led_handle, 255, 255, 0);  // 黄色：WiFi 连接中 / AP 配网模式
        break;
    case 2:
        led_set_all(g_led_handle, 0, 255, 0);    // 绿色：WiFi 已连接
        break;
    default:
        break;
    }
}

/**
 * @brief WiFi 状态变化回调（由 wifi_manager 调用）
 * @param status 0=断开(红), 1=连接中(黄), 2=已连接(绿)
 *
 * 在以下时机被调用：
 * - WiFi 开始连接 → status=1
 * - WiFi 获取 IP → status=2
 * - WiFi 断开连接 → status=0
 */
static void wifi_status_callback(int status)
{
    g_wifi_status = status;
    refresh_led();
}


/**
 * @brief 按钮扫描任务（FreeRTOS 任务，常驻循环）
 * @param arg 未使用
 *
 * 仅监控 BOOT 按钮（GPIO 0，低电平有效，内部上拉）。
 *
 * 操作判定逻辑：
 * ┌──────────────┬────────────┬──────────────────────────────┐
 * │ 操作         │ 按住时长    │ 行为                          │
 * ├──────────────┼────────────┼──────────────────────────────┤
 * │ 单击         │ < 1秒      │ LED 变蓝色（提示反馈）         │
 * │ 长按         │ 1~5秒      │ 切换 LED 开关（亮↔灭）        │
 * │ 超长按       │ ≥ 5秒      │ 清除 WiFi 配置→紫色→重启      │
 * └──────────────┴────────────┴──────────────────────────────┘
 *
 * 长按和超长按的优先级：按住过程中先检测是否达到1秒（长按），
 * 继续按住则检测是否达到5秒（超长按），超长按优先于长按。
 */
static void button_task(void *arg)
{
    (void)arg;

    // 配置 BOOT 按钮 GPIO 为输入模式，启用内部上拉（按钮按下时接 GND = 低电平）
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),    // GPIO 0
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,               // 内部上拉，默认高电平
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,                  // 不使用中断，轮询方式
    };
    gpio_config(&btn_conf);

    const TickType_t SHORT_PRESS_MS = 1000;   // 短按/长按分界线：1秒
    const TickType_t RESET_PRESS_MS = 5000;   // 超长按分界线：5秒（清除 WiFi）
    bool was_pressed = false;                  // 上一轮扫描的按钮状态

    while (true)
    {
        int level = gpio_get_level(static_cast<gpio_num_t>(BOOT_BUTTON_GPIO));
        bool pressed = (level == 0);           // 低电平 = 按下

        // 检测下降沿：按钮刚被按下
        if (pressed && !was_pressed)
        {
            // 记录按下时刻，用于计算按住时长
            TickType_t press_start = xTaskGetTickCount();

            // 轮询等待按钮松开或达到某个时间阈值
            while (gpio_get_level(static_cast<gpio_num_t>(BOOT_BUTTON_GPIO)) == 0)
            {
                vTaskDelay(pdMS_TO_TICKS(50));  // 50ms 轮询间隔
                TickType_t elapsed = (xTaskGetTickCount() - press_start) * portTICK_PERIOD_MS;

                // ---- 超长按 ≥5秒：清除 WiFi 配置 ----
                if (elapsed >= RESET_PRESS_MS)
                {
                    if (g_led_handle)
                    {
                        g_led_off = false;
                        led_set_all(g_led_handle, 128, 0, 128); // 紫色：正在清除 WiFi
                    }
                    wifi_clear_saved_config();
                    ESP_LOGI(TAG, "超长按BOOT按钮5秒，WiFi配置已清除");

                    // 等待按钮松开，避免重启过程中 GPIO 状态不确定
                    while (gpio_get_level(static_cast<gpio_num_t>(BOOT_BUTTON_GPIO)) == 0)
                    {
                        vTaskDelay(pdMS_TO_TICKS(50));
                    }
                    was_pressed = true;

                    // 清除完成后延迟重启，给用户看到紫色 LED 反馈
                    ESP_LOGI(TAG, "3秒后重启设备...");
                    vTaskDelay(pdMS_TO_TICKS(3000));
                    esp_restart();
                    break;
                }

                // ---- 长按 ≥1秒：切换 LED 开关 ----
                // 条件 elapsed < SHORT_PRESS_MS + 100 确保只触发一次
                if (elapsed >= SHORT_PRESS_MS && elapsed < SHORT_PRESS_MS + 100)
                {
                    g_led_off = !g_led_off;
                    refresh_led();
                    ESP_LOGI(TAG, "长按BOOT按钮，LED %s", g_led_off ? "关闭" : "开启");

                    // 等待按钮松开，但继续检测是否达到 5 秒（超长按优先）
                    while (gpio_get_level(static_cast<gpio_num_t>(BOOT_BUTTON_GPIO)) == 0)
                    {
                        vTaskDelay(pdMS_TO_TICKS(50));
                        TickType_t elapsed2 = (xTaskGetTickCount() - press_start) * portTICK_PERIOD_MS;

                        // 长按过程中达到 5 秒，升级为超长按
                        if (elapsed2 >= RESET_PRESS_MS)
                        {
                            if (g_led_handle)
                            {
                                g_led_off = false;
                                led_set_all(g_led_handle, 128, 0, 128); // 紫色
                            }
                            wifi_clear_saved_config();
                            ESP_LOGI(TAG, "超长按BOOT按钮5秒，WiFi配置已清除");

                            while (gpio_get_level(static_cast<gpio_num_t>(BOOT_BUTTON_GPIO)) == 0)
                            {
                                vTaskDelay(pdMS_TO_TICKS(50));
                            }
                            was_pressed = true;

                            ESP_LOGI(TAG, "3秒后重启设备...");
                            vTaskDelay(pdMS_TO_TICKS(3000));
                            esp_restart();
                            break;
                        }
                    }
                    break;
                }
            }

            // ---- 单击 <1秒：LED 变蓝色（仅提示反馈，不影响 WiFi 状态灯逻辑）----
            if (!was_pressed || gpio_get_level(static_cast<gpio_num_t>(BOOT_BUTTON_GPIO)) != 0)
            {
                TickType_t elapsed = (xTaskGetTickCount() - press_start) * portTICK_PERIOD_MS;
                if (elapsed < SHORT_PRESS_MS && g_led_handle && !g_led_off)
                {
                    led_set_all(g_led_handle, 0, 0, 255); // 蓝色：单击反馈
                    ESP_LOGI(TAG, "单击BOOT按钮，LED显示蓝色");
                }
            }
        }

        was_pressed = pressed;
        vTaskDelay(pdMS_TO_TICKS(50));  // 50ms 轮询间隔
    }
}

/**
 * @brief WiFi 初始化完成后同步 LED 状态
 *
 * wifi_init() 完成后调用此函数，根据实际 WiFi 状态设置 LED 颜色：
 * - 已连接（STA 拿到 IP）→ 绿色
 * - AP 配网模式 → 黄色
 * - 未连接 → 红色
 */
void board_sync_wifi_led(void)
{
    wifi_status_t status{};
    wifi_get_status(&status);

    if (status.connected)
    {
        g_wifi_status = 2;  // 绿色
    }
    else if (status.ap_mode_active)
    {
        g_wifi_status = 1;  // 黄色
    }
    else
    {
        g_wifi_status = 0;  // 红色
    }

    refresh_led();
}
