cmake_minimum_required(VERSION 3.13)
set(CMAKE_C_STANDARD 11)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================

set(PICO_BOARD pico CACHE STRING "Board type")  # plain pico: WiFi is the ESP8285, not a CYW43
include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)
project(pico_wifi_display C CXX ASM)
pico_sdk_init()

add_executable(pico_wifi_display main.c)
pico_generate_pio_header(pico_wifi_display ${CMAKE_CURRENT_LIST_DIR}/ws2812.pio)
target_link_libraries(pico_wifi_display pico_stdlib hardware_pio hardware_uart)
pico_enable_stdio_usb(pico_wifi_display 1)
pico_enable_stdio_uart(pico_wifi_display 0)  # UART0 belongs to the ESP8285
pico_add_extra_outputs(pico_wifi_display)
