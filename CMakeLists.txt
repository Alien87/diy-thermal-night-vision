cmake_minimum_required(VERSION 3.16.0)

# PlatformIO passes SDKCONFIG=<project>/sdkconfig.<env>; layer board overrides
if(SDKCONFIG MATCHES "sdkconfig\.esp32-p4-pico$")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.pico")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ESP32-P4_Thermal)
