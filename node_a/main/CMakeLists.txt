idf_component_register(
    SRCS
        "app_main.c"
        "queues.c"
        "state_cache.c"
        "nvs_store.c"
        "arbiter.c"
        "ble_owner.c"
        "decoder.c"
        "tunnel_srv.c"
        "mqtt_task.c"
        "measure.c"
        "supervisor.c"
    INCLUDE_DIRS "."
    REQUIRES
        common jk_proto net_util
        nvs_flash esp_wifi esp_event esp_netif esp_timer esp_task_wdt
        mqtt bt json lwip
)
