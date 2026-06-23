# main 组件注册
#
# 源文件：
#   - main.cpp : 程序入口，USB/IP 服务器启动，内存监控
#   - board.cpp : 板级管理，LED/按钮/USB Host 初始化
#
# 依赖组件：
#   - asio        : 异步网络库（USB/IP TCP 通信）
#   - spdlog      : 日志库
#   - nvs_flash   : 非易失性存储（WiFi 配置持久化）
#   - esp_wifi    : WiFi 驱动
#   - pthread     : POSIX 线程支持（C++ std::thread 底层依赖）
#   - usb         : USB Host 驱动
#   - usbipdcpp   : USB/IP 协议栈核心库
#   - board_utils : 板级工具（LED/WiFi/按钮/GPIO TCP）
#   - driver      : GPIO 等 ESP 外设驱动
idf_component_register(
        SRCS "main.cpp" "board.cpp"
        INCLUDE_DIRS "."
        PRIV_REQUIRES asio spdlog nvs_flash esp_wifi pthread usb usbipdcpp board_utils driver)

# 忽略结构体初始化时部分字段未设置的警告（C 风格驱动 API 常见）
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
#target_compile_definitions(main PRIVATE SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE)

# 全局编译定义：
#   - SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO : spdlog 编译期日志级别（Release 模式）
#     调试时改为 SPDLOG_LEVEL_TRACE 可输出所有日志
#   - NDEBUG : 禁用 assert，Release 优化
add_compile_definitions(
#        SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE
        SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO
        NDEBUG
)
