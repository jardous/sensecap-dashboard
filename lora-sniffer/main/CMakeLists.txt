idf_component_register(SRCS "../src/main.cpp" "../src/radio.cpp" "../src/server.cpp"
    "../src/serial_console.cpp" "../src/serial_diagnostics.cpp"
    "../../shared/lora/protocol.cpp" "../../shared/lora/classifier.cpp"
    INCLUDE_DIRS "../include" "../../shared"
    REQUIRES RadioLib driver esp_timer nvs_flash esp_wifi esp_event esp_netif esp_http_server json mdns)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-unused-parameter "$<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>")
