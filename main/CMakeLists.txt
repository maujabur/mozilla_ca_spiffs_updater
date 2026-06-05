idf_component_register(
    SRCS
        "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        ca_manager
        cjson
        esp_event
        esp_netif
        esp_wifi
        nvs_flash
)
