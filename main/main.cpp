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

constexpr std::uint16_t listening_port = 3240;

void init() {
    ESP_LOGI(TAG, "初始化所有设备");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "初始化nvs结束");

    if (CONFIG_LOG_MAXIMUM_LEVEL > CONFIG_LOG_DEFAULT_LEVEL) {
        esp_log_level_set("wifi", static_cast<esp_log_level_t>(CONFIG_LOG_MAXIMUM_LEVEL));
    }

    board_setup();
    wifi_init();
    board_sync_wifi_led();
    init_usb_host();
}

using namespace usbipdcpp;

int main() {
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

    spdlog::set_level(spdlog::level::info);

    Esp32Server server;
    server.init_client();

    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), listening_port};
    server.start(endpoint);

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
