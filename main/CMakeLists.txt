idf_component_register(SRCS
    "app/main.cpp" "config/settings.cpp" "sensors/uart.cpp"
    "display/port.c" "network/services.cpp" "monitor/hosts.cpp"
    "ui/dashboard.cpp" "core/model.cpp"
    INCLUDE_DIRS "." REQUIRES bsp lvgl nvs_flash esp_wifi esp_event esp_netif
    esp_timer mqtt esp_http_client json lwip driver esp-tls)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
set_target_properties(${COMPONENT_LIB} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_compile_options(${COMPONENT_LIB} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>")
