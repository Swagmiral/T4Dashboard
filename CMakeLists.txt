cmake_minimum_required(VERSION 3.16)

# Fast boot: include only what we need
set(COMPONENTS 
    main
    esp_driver_gpio
    esp_driver_i2c
    esp_driver_spi
    esp_lcd
    esp_timer
    nvs_flash
    freertos
    log
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(t4dashboard)
