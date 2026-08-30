idf_component_register(
    SRCS "http_server.c"
    INCLUDE_DIRS "include" "../../include"
    PRIV_REQUIRES esp_http_server esp_wifi esp_netif lwip mbedtls esp_timer
)
