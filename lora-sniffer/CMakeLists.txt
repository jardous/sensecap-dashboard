cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../vendor/RadioLib"
    "${CMAKE_CURRENT_LIST_DIR}/../vendor/esp-protocols/components/mdns")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(PROJECT_VER "0.2.0")
project(lora_sniffer)
