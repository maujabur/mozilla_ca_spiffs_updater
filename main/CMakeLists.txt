idf_component_register(
    SRCS
        "main.c"
        "ca_manager.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        esp_event
        esp_http_client
        esp_netif
        esp_wifi
        mbedtls
        nvs_flash
        spiffs
)
