# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

set(app_sources "main.c" "classhub.cpp" "rc522_reader.c")

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        esp_driver_gpio
        esp_driver_ledc
        esp_driver_i2c
        esp_driver_pcnt
        esp_driver_spi
        esp_rom
        nvs_flash esp_psram esp32-camera
        esp_timer
        esp_wifi esp_event esp_netif esp_http_server
        mqtt cjson qrcode lwip mbedtls
        app_update
)
