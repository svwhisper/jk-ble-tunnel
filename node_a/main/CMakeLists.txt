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
        "log_udp.c"
        "display.c"
    INCLUDE_DIRS "."
    REQUIRES
        common secret jk_proto net_util oled ota
        nvs_flash esp_wifi esp_event esp_netif esp_timer esp_system
        mqtt bt json lwip
)
