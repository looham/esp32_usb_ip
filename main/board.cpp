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

static led_handle_t *g_led_handle = nullptr;
static volatile bool g_led_off = false;         // 长按关闭LED标志
static volatile int g_wifi_status = 0;          // 当前WiFi状态

std::thread usb_host_event_thread;


static void refresh_led(void);
static void wifi_status_callback(int status);
static void button_task(void *arg);

void board_setup(void)
{
    // 初始化LED（WS2812 RGB LED）
    led_config_t led_config = {
        .gpio_num = LED_PIN,
        .led_num = 1,
        .brightness = 80, // 亮度适中
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

    wifi_set_status_callback(wifi_status_callback); // 初始化 WIFI状态灯
    xTaskCreate(button_task, "button_task", 4096, NULL, 5, NULL); // 初始化 按钮
    gpio_tcp_server_init(g_led_handle); // 初始化 GPIO TCP
}


/** 初始化 USB 设备 */
void init_usb_host() {
    ESP_LOGI(TAG, "Installing USB Host Library");
    usb_host_config_t host_config = {
            .skip_phy_setup = false,
            .intr_flags = ESP_INTR_FLAG_LEVEL3,
            .enum_filter_cb = nullptr,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));

    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.prio = 10;
    cfg.pin_to_core = 1;
    cfg.thread_name = "usb_host_event_thread";
    cfg.stack_size = 4096;
    esp_pthread_set_cfg(&cfg);

    usb_host_event_thread = std::thread([]() {
        bool has_clients = true;
        bool has_devices = false;
        while (has_clients) {
            uint32_t event_flags;
            ESP_ERROR_CHECK(usb_host_lib_handle_events(portMAX_DELAY, &event_flags));
            if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
                ESP_LOGI(TAG, "Get FLAGS_NO_CLIENTS");
                if (ESP_OK == usb_host_device_free_all()) {
                    ESP_LOGI(TAG, "All devices marked as free, no need to wait FLAGS_ALL_FREE event");
                    has_clients = false;
                }
                else {
                    ESP_LOGI(TAG, "Wait for the FLAGS_ALL_FREE");
                    has_devices = true;
                }
            }
            if (has_devices && event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
                ESP_LOGI(TAG, "Get FLAGS_ALL_FREE");
                has_clients = false;
            }
        }
        ESP_LOGI(TAG, "No more clients and devices, uninstall USB Host library");
        ESP_ERROR_CHECK(usb_host_uninstall());
    });
    auto default_cfg = esp_pthread_get_default_config();
    esp_pthread_set_cfg(&default_cfg);
}


// 根据当前状态刷新LED颜色
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
        led_set_all(g_led_handle, 255, 0, 0); // 红色
        break;
    case 1:
        led_set_all(g_led_handle, 255, 255, 0); // 黄色
        break;
    case 2:
        led_set_all(g_led_handle, 0, 255, 0); // 绿色
        break;
    default:
        break;
    }
}

// WiFi状态回调：0=启动/断开(红色), 1=连接中(黄色), 2=已连接(绿色)
static void wifi_status_callback(int status)
{
    g_wifi_status = status;
    refresh_led();
}


static void button_task(void *arg)
{
    (void)arg;

    // 配置BOOT按钮GPIO为输入，启用内部上拉
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_conf);

    const TickType_t SHORT_PRESS_MS = 1000;  // 短按/长按1秒分界
    const TickType_t RESET_PRESS_MS = 5000;  // 5秒超长按分界（清除WiFi）
    bool was_pressed = false;

    while (true)
    {
        int level = gpio_get_level(static_cast<gpio_num_t>(BOOT_BUTTON_GPIO));
        bool pressed = (level == 0); // BOOT按钮按下为低电平

        if (pressed && !was_pressed)
        {
            // 按钮刚按下，记录时间
            TickType_t press_start = xTaskGetTickCount();

            // 等待松开或超时
            while (gpio_get_level(static_cast<gpio_num_t>(BOOT_BUTTON_GPIO)) == 0)
            {
                vTaskDelay(pdMS_TO_TICKS(50));
                TickType_t elapsed = (xTaskGetTickCount() - press_start) * portTICK_PERIOD_MS;

                if (elapsed >= RESET_PRESS_MS)
                {
                    // 超长按5秒：清除WiFi配置，显示紫色
                    if (g_led_handle)
                    {
                        g_led_off = false;
                        led_set_all(g_led_handle, 128, 0, 128); // 紫色
                    }
                    wifi_clear_saved_config();
                    ESP_LOGI(TAG, "超长按BOOT按钮5秒，WiFi配置已清除");

                    // 等待按钮松开
                    while (gpio_get_level(static_cast<gpio_num_t>(BOOT_BUTTON_GPIO)) == 0)
                    {
                        vTaskDelay(pdMS_TO_TICKS(50));
                    }
                    was_pressed = true;

                    // 清除后重启设备
                    ESP_LOGI(TAG, "3秒后重启设备...");
                    vTaskDelay(pdMS_TO_TICKS(3000));
                    esp_restart();
                    break;
                }

                if (elapsed >= SHORT_PRESS_MS && elapsed < SHORT_PRESS_MS + 100)
                {
                    // 长按1秒：切换LED开关
                    g_led_off = !g_led_off;
                    refresh_led();
                    ESP_LOGI(TAG, "长按BOOT按钮，LED %s", g_led_off ? "关闭" : "开启");

                    // 等待按钮松开，但要继续检测5秒
                    while (gpio_get_level(static_cast<gpio_num_t>(BOOT_BUTTON_GPIO)) == 0)
                    {
                        vTaskDelay(pdMS_TO_TICKS(50));
                        TickType_t elapsed2 = (xTaskGetTickCount() - press_start) * portTICK_PERIOD_MS;
                        if (elapsed2 >= RESET_PRESS_MS)
                        {
                            // 5秒超长按
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

            // 如果不是长按（按钮已松开且未超时），则是单击
            if (!was_pressed || gpio_get_level(static_cast<gpio_num_t>(BOOT_BUTTON_GPIO)) != 0)
            {
                TickType_t elapsed = (xTaskGetTickCount() - press_start) * portTICK_PERIOD_MS;
                if (elapsed < SHORT_PRESS_MS && g_led_handle && !g_led_off)
                {
                    led_set_all(g_led_handle, 0, 0, 255); // 蓝色
                    ESP_LOGI(TAG, "单击BOOT按钮，LED显示蓝色");
                }
            }
        }

        was_pressed = pressed;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void board_sync_wifi_led(void)
{
    wifi_status_t status{};
    wifi_get_status(&status);

    if (status.connected)
    {
        g_wifi_status = 2;
    }
    else if (status.ap_mode_active)
    {
        g_wifi_status = 1;
    }
    else
    {
        g_wifi_status = 0;
    }

    refresh_led();
}
