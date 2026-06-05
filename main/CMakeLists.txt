idf_component_register(
    SRCS
        "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        ca_manager
        esp_event
        esp_netif
        esp_wifi
        nvs_flash
)
