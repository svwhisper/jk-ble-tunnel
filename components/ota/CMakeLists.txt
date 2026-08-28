idf_component_register(
    SRCS "ota.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server app_update esp_app_format esp_partition esp_timer esp_system
)
