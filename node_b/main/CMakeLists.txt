idf_component_register(
    SRCS
        "app_main.c"
        "nb_state.c"
        "tunnel_cli.c"
        "adv_mgr.c"
        "ble_periph.c"
        "supervisor.c"
    INCLUDE_DIRS "."
    REQUIRES
        common secret net_util
        nvs_flash esp_wifi esp_event esp_netif esp_timer esp_task_wdt
        bt lwip
)
