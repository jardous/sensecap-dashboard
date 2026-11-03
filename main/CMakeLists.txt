idf_component_register(SRCS
    "app/main.cpp" "config/settings.cpp" "config/usb_setup.cpp" "sensors/uart.cpp"
    "display/port.c" "display/screen_button.cpp" "network/services.cpp" "monitor/hosts.cpp"
    "ui/dashboard.cpp" "ui/lora_page.cpp" "core/model.cpp" "lora/client.cpp"
    "../shared/lora/protocol.cpp" "../shared/lora/classifier.cpp"
    INCLUDE_DIRS "." "../shared" REQUIRES bsp lvgl nvs_flash esp_wifi esp_event esp_netif
    esp_timer mqtt esp_http_client json lwip driver esp-tls esp_websocket_client)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
set_target_properties(${COMPONENT_LIB} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_compile_options(${COMPONENT_LIB} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>")
