idf_component_register(
        SRCS "main.cpp" "board.cpp"
        INCLUDE_DIRS "."
        PRIV_REQUIRES asio spdlog nvs_flash esp_wifi pthread usb usbipdcpp board_utils driver)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
#target_compile_definitions(main PRIVATE SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE)
add_compile_definitions(
#        SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE
        SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO
        NDEBUG
)