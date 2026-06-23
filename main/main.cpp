/**
 * @file main.cpp
 * @brief ESP32 USB/IP 网关主程序入口
 *
 * 程序启动流程：
 *   1. app_main() → 创建主线程（Core 1, prio 5, 8KB 栈）
 *   2. init()     → NVS 初始化 → 板级初始化（LED/按钮/GPIO TCP）→ WiFi 连接 → USB Host 初始化
 *   3. main()     → 创建 Esp32Server → 注册 USB Host 客户端 → 监听 TCP 3240 端口 → 内存监控循环
 *
 * USB/IP 服务：ESP32 作为 USB/IP 服务器，将物理 USB 设备通过网络共享给远程主机。
 * 远程主机通过 usbip 客户端工具 attach 到 TCP 3240 端口即可使用设备。
 */

#include "sdkconfig.h"

#include <iostream>
#include <thread>

#include <nvs_flash.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_pthread.h>

#include <lwip/sys.h>
#include <lwip/sockets.h>

#include <asio.hpp>
#include <spdlog/spdlog.h>

#include "esp32_handler/Esp32Server.h"
#include "wifi_manager.h"
#include "board.h"

using namespace std;

auto TAG = "MAIN";

/** USB/IP 协议标准监听端口 */
constexpr std::uint16_t listening_port = 3240;

/**
 * @brief 系统初始化（NVS → 板级 → WiFi → USB Host）
 *
 * 初始化顺序说明：
 * - NVS 先初始化，因为 WiFi 配置存储在 NVS 中
 * - 板级初始化在 WiFi 之前，这样 LED 可以及时反映 WiFi 状态
 * - USB Host 最后初始化，确保网络就绪后再接受 USB/IP 客户端连接
 */
void init() {
    ESP_LOGI(TAG, "初始化所有设备");

    // 初始化 NVS（非易失性存储），WiFi 配置等依赖此分区
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS 分区已满或版本不匹配，擦除后重新初始化
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "初始化nvs结束");

    // 如果最大日志级别高于默认级别，提升 WiFi 模块的日志输出
    if (CONFIG_LOG_MAXIMUM_LEVEL > CONFIG_LOG_DEFAULT_LEVEL) {
        esp_log_level_set("wifi", static_cast<esp_log_level_t>(CONFIG_LOG_MAXIMUM_LEVEL));
    }

    board_setup();          // LED / 按钮 / GPIO TCP 初始化
    wifi_init();            // WiFi STA/AP 配网
    board_sync_wifi_led();  // WiFi 初始化后同步 LED 状态
    init_usb_host();        // USB Host 驱动初始化
}

using namespace usbipdcpp;

/**
 * @brief 主业务逻辑：启动 USB/IP 服务器并监控内存
 *
 * 1. 打印当前 WiFi 状态
 * 2. 创建 Esp32Server，注册 USB Host 客户端
 * 3. 在 TCP 3240 端口上监听 USB/IP 连接
 * 4. 每 5 秒打印一次内存使用情况（内部 RAM、DMA、PSRAM）
 *    - Free: 当前空闲堆
 *    - Min: 历史最低空闲堆（水位线，用于检测内存泄漏）
 *    - DMA free/min: DMA 可用的内部 RAM
 *    - PSRAM free/min: 外部 SPIRAM
 *    - DMA max block: DMA 区域最大连续空闲块
 */
int main() {
    // 打印 WiFi 连接状态
    wifi_status_t wifi_status{};
    wifi_get_status(&wifi_status);
    if (wifi_status.connected) {
        ESP_LOGI(TAG, "WiFi 已连接: %s", wifi_status.ssid);
    }
    else if (wifi_status.ap_mode_active) {
        ESP_LOGI(TAG, "AP 配网模式运行中，SSID: %s", CONFIG_AP_SSID);
    }
    else {
        ESP_LOGW(TAG, "WiFi 未连接");
    }

    // 设置 spdlog 全局日志级别
    spdlog::set_level(spdlog::level::info);

    // 创建 USB/IP 服务器并注册 USB Host 客户端（用于接收设备热插拔事件）
    Esp32Server server;
    server.init_client();

    // 监听所有网卡的 TCP 3240 端口（USB/IP 协议标准端口）
    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), listening_port};
    server.start(endpoint);

    // 主循环：定期打印内存使用情况，用于运行时监控
    while (true) {
        std::this_thread::sleep_for(chrono::seconds(5));
        ESP_LOGI(TAG, "Free: %.2f MB, Min: %.2f MB, DMA free: %.2f MB, DMA min: %.2f MB, PSRAM free: %.2f MB, PSRAM min: %.2f MB, DMA max block: %.2f MB",
            bytes_to_mb(esp_get_free_heap_size()),
            bytes_to_mb(esp_get_minimum_free_heap_size()),
            bytes_to_mb(heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)),
            bytes_to_mb(heap_caps_get_minimum_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)),
            bytes_to_mb(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
            bytes_to_mb(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM)),
            bytes_to_mb(heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL))
        );
    }

    server.stop();
    return 0;
}

/**
 * @brief ESP-IDF 程序入口（FreeRTOS 任务的 C 入口）
 *
 * ESP-IDF 的 app_main 运行在 main 任务中，栈空间有限（默认约 3.5KB），
 * 因此在此创建一个独立线程来运行 C++ main()，拥有更大的栈空间和更高的优先级。
 *
 * 线程配置：
 * - 绑定到 Core 1（Core 0 留给 WiFi/LWIP 和 GPIO TCP 服务）
 * - 优先级 5（高于默认的 1，低于 USB Host 事件的 10）
 * - 栈大小 8KB（asio/spdlog 等 C++ 库需要较大栈空间）
 */
extern "C" void app_main(void) {
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.pin_to_core = 1;
    cfg.prio = 5;
    cfg.stack_size = 8192;
    cfg.thread_name = "main_thread";
    esp_pthread_set_cfg(&cfg);

    std::thread main_thread([&]() {
        ESP_LOGI(TAG, "启动主线程main函数");

        init();
        main();
    });
    main_thread.join();
}
