cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/vendor/seeed-esp32/components/bsp"
    "${CMAKE_CURRENT_LIST_DIR}/vendor/seeed-esp32/components/bus"
    "${CMAKE_CURRENT_LIST_DIR}/vendor/seeed-esp32/components/i2c_devices"
    "${CMAKE_CURRENT_LIST_DIR}/vendor/seeed-esp32/components/iot_button"
    "${CMAKE_CURRENT_LIST_DIR}/vendor/seeed-esp32/components/lora"
    "${CMAKE_CURRENT_LIST_DIR}/vendor/seeed-esp32/components/lvgl")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(PROJECT_VER "0.1.0")
project(sensecap_dashboard VERSION 0.1.0)
# Seeed's pinned C drivers predate IDF's GCC 12 format diagnostics. Keep these
# visible as warnings only in vendor targets; application diagnostics stay strict.
foreach(component bsp bus i2c_devices iot_button)
    idf_component_get_property(vendor_target ${component} COMPONENT_LIB)
    target_compile_options(${vendor_target} PRIVATE
        -Wno-error=format -Wno-error=maybe-uninitialized)
endforeach()
