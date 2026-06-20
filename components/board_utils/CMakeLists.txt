idf_component_register(
    SRCS 
        "src/button_manager.c" 
        "src/led_manager.c" 
        "src/wifi_manager.c"
        "src/gpio_tcp_server.c"
    INCLUDE_DIRS "include"
    REQUIRES
        led_strip      # LED灯带组件
        esp_wifi       # wifi组件依赖
        nvs_flash
        esp_netif
        lwip
        esp_event      # 配网组件
        esp_http_server
        driver
        esp_driver_gpio
)