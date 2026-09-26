idf_component_register(
    SRCS "freya_coprocessor.c" "term_server.c" "web_server.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio esp_driver_spi esp_event esp_http_client esp_netif
             esp-tls esp_wifi lwip mbedtls nvs_flash
)
